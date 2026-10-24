#include "loudness.h"

#include <algorithm>
#include <cmath>

namespace mf {

namespace {

constexpr double kPiD = 3.14159265358979323846;
constexpr double kSr = 44100.0;

double toLufs(double meanSquare) { return meanSquare > 1e-20 ? -0.691 + 10.0 * std::log10(meanSquare) : Loudness::kSilence; }

} // namespace

void Loudness::design(Biquad& shelf, Biquad& high) {
    {   // the high shelf: +4 dB from about 1.7 kHz (libebur128's derivation)
        const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan(kPiD * f0 / kSr), vh = std::pow(10.0, g / 20.0), vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf.b0 = (vh + vb * k / q + k * k) / a0;
        shelf.b1 = 2.0 * (k * k - vh) / a0;
        shelf.b2 = (vh - vb * k / q + k * k) / a0;
        shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        shelf.a2 = (1.0 - k / q + k * k) / a0;
    }
    {   // the high-pass at 38 Hz
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan(kPiD * f0 / kSr), a0 = 1.0 + k / q + k * k;
        high.b0 = 1.0;
        high.b1 = -2.0;
        high.b2 = 1.0;
        high.a1 = 2.0 * (k * k - 1.0) / a0;
        high.a2 = (1.0 - k / q + k * k) / a0;
    }
}

Loudness::Loudness() {
    design(shelf_, high_);
    reset();
}

float Loudness::weightingDb(float hz) {
    Biquad f[2];
    design(f[0], f[1]);
    const double w = 2.0 * kPiD * hz / kSr, c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
    double db = 0.0;
    for (const Biquad& b : f) {   // |B(e^jw)|^2 / |A(e^jw)|^2
        const double nr = b.b0 + b.b1 * c1 + b.b2 * c2, ni = -(b.b1 * s1 + b.b2 * s2);
        const double dr = 1.0 + b.a1 * c1 + b.a2 * c2, di = -(b.a1 * s1 + b.a2 * s2);
        db += 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
    }
    return static_cast<float>(db);
}

void Loudness::reset() {
    for (Biquad* b : {&shelf_, &high_})
        for (int c = 0; c < 2; ++c) b->z1[c] = b->z2[c] = 0.0;
    acc_ = 0.0;
    inBlock_ = 0;
    std::fill(blocks_, blocks_ + kShort, 0.0);
    blockW_ = filled_ = 0;
    std::fill(count_, count_ + kBins, 0u);
    std::fill(energy_, energy_ + kBins, 0.0);
    tp_.reset();
    peak_ = 0.0f;
}

void Loudness::process(const float* L, const float* R, int n) {
    for (int i = 0; i < n; ++i) {
        const float l = ef::sanitize(L[i]), r = ef::sanitize(R[i]);
        peak_ = std::max(peak_, tp_.push(l, r));
        const double kl = high_.run(0, shelf_.run(0, l)), kr = high_.run(1, shelf_.run(1, r));
        acc_ += kl * kl + kr * kr;
        if (++inBlock_ < kBlock) continue;

        blocks_[blockW_] = acc_ / kBlock;
        blockW_ = blockW_ + 1 == kShort ? 0 : blockW_ + 1;
        filled_ = std::min(filled_ + 1, kShort);
        acc_ = 0.0;
        inBlock_ = 0;
        for (Biquad* b : {&shelf_, &high_})   // decayed state to zero, not on into denormals
            for (int c = 0; c < 2; ++c) {
                if (std::fabs(b->z1[c]) < 1e-30) b->z1[c] = 0.0;
                if (std::fabs(b->z2[c]) < 1e-30) b->z2[c] = 0.0;
            }

        if (filled_ >= 4) {   // a 400 ms window ends here: into the histogram, if above -70
            const double z = meanOf(4);
            const double lu = toLufs(z);
            if (lu > -70.0) {
                const int bin = std::min(kBins - 1, static_cast<int>((lu + 70.0) * 10.0));
                ++count_[bin];
                energy_[bin] += z;
            }
        }
    }
}

double Loudness::meanOf(int blocks) const {
    const int n = std::min(blocks, filled_);
    if (n == 0) return 0.0;
    double s = 0.0;
    for (int j = 1; j <= n; ++j) s += blocks_[(blockW_ - j + kShort) % kShort];
    return s / n;
}

float Loudness::momentary() const { return filled_ >= 4 ? static_cast<float>(toLufs(meanOf(4))) : kSilence; }
float Loudness::shortTerm() const { return filled_ >= 1 ? static_cast<float>(toLufs(meanOf(kShort))) : kSilence; }

float Loudness::integrated() const {
    uint64_t n = 0;
    double e = 0.0;
    for (int b = 0; b < kBins; ++b) {
        n += count_[b];
        e += energy_[b];
    }
    if (n == 0) return kSilence;
    const double gate = toLufs(e / static_cast<double>(n)) - 10.0;
    const int first = std::max(0, static_cast<int>(std::ceil((gate + 70.0) * 10.0)));
    n = 0;
    e = 0.0;
    for (int b = first; b < kBins; ++b) {
        n += count_[b];
        e += energy_[b];
    }
    return n ? static_cast<float>(toLufs(e / static_cast<double>(n))) : kSilence;
}

float Loudness::truePeakDb() const { return peak_ > 1e-10f ? 20.0f * std::log10(peak_) : kSilence; }

} // namespace mf
