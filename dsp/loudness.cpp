#include "loudness.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
    // The same two sections as SVFs (g = tan(pi f0 / rate), k = 1 / q): the shelf is
    // (vh s^2 + vb k s + 1) / (s^2 + k s + 1), mixed v0 vh, v1 k (vb - vh), v2 (1 - vh); the high-pass
    // s^2 / (..) times a0 = 1 + g k + g^2, because the standard's biquad keeps its numerator 1, -2, 1
    // undivided: its passband sits 0.047 dB up, and BS.1770's -0.691 counts on that.
    {
        const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
        const double vh = std::pow(10.0, g / 20.0), vb = std::pow(vh, 0.4996667741545416), k = 1.0 / q;
        shelfA_ = ef::SvfUpdate::of(ef::splat2(static_cast<float>(std::tan(kPiD * f0 / kSr))), ef::splat2(static_cast<float>(k)));
        shelfM_[0] = ef::splat2(static_cast<float>(vh));
        shelfM_[1] = ef::splat2(static_cast<float>(k * (vb - vh)));
        shelfM_[2] = ef::splat2(static_cast<float>(1.0 - vh));
    }
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773, k = 1.0 / q, g = std::tan(kPiD * f0 / kSr);
        const double a0 = 1.0 + g * k + g * g;
        highA_ = ef::SvfUpdate::of(ef::splat2(static_cast<float>(g)), ef::splat2(static_cast<float>(k)));
        highM_[0] = ef::splat2(static_cast<float>(a0));
        highM_[1] = ef::splat2(static_cast<float>(-k * a0));
        highM_[2] = ef::splat2(static_cast<float>(-a0));
    }
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
    shelf_.clear();
    high_.clear();
    acc_ = 0.0;
    inBlock_ = 0;
    std::fill(blocks_, blocks_ + kShort, 0.0);
    blockW_ = filled_ = 0;
    std::fill(count_, count_ + kBins, 0u);
    std::fill(energy_, energy_ + kBins, 0.0);
    tp_.reset();
    peak_ = 0.0f;
    momentary_ = shortTerm_ = integrated_ = kSilence;
}

void Loudness::process(const float* L, const float* R, int n) {
    peak_ = std::max(peak_, tp_.maxOf(L, R, n));
    alignas(16) float l[TruePeak::kBlock], r[TruePeak::kBlock];
    for (int pos = 0; pos < n; pos += TruePeak::kBlock) {   // clamped as the detector reads it: nothing overflows
        const int len = std::min(TruePeak::kBlock, n - pos);
        copy(l, L + pos, len);
        copy(r, R + pos, len);
        for (int i = len; i < (len + 3) / 4 * 4; ++i) l[i] = r[i] = 0.0f;   // clampIn() reads whole fours
        clampIn(l, len, TruePeak::kMaxIn);
        clampIn(r, len, TruePeak::kMaxIn);
        for (int i = 0; i < len;) {   // up to each 100 ms block's end
            const int m = std::min(len - i, kBlock - inBlock_);
            weight(l + i, r + i, m);
            i += m;
            if ((inBlock_ += m) == kBlock) endBlock();
        }
    }
}

// K-weighted, squared and summed: in float over a few samples, then into the block's double.
void Loudness::weight(const float* l, const float* r, int n) {
    ef::SvfState s = shelf_, h = high_;   // locals: kept in registers across the loop
    ef::f2 sum = ef::splat2(0.0f);
    for (int i = 0; i < n; ++i) {
        const ef::f2 k = h.tick(s.tick(ef::load2(l + i, r + i), shelfA_, shelfM_), highA_, highM_);
        sum += k * k;
    }
    shelf_ = s;
    high_ = h;
#if EF_NEON
    acc_ += static_cast<double>(vget_lane_f32(sum, 0)) + static_cast<double>(vget_lane_f32(sum, 1));
#else
    acc_ += static_cast<double>(sum[0]) + static_cast<double>(sum[1]);
#endif
}

// A 100 ms block ends: into the short-term ring, and its 400 ms window into the histogram. The
// readings change only here, so they are worked out here once, not on every look.
void Loudness::endBlock() {
    blocks_[blockW_] = acc_ / kBlock;
    blockW_ = blockW_ + 1 == kShort ? 0 : blockW_ + 1;
    filled_ = std::min(filled_ + 1, kShort);
    acc_ = 0.0;
    inBlock_ = 0;
    shelf_.flushTiny();   // decayed state to zero, not on into denormals
    high_.flushTiny();

    if (filled_ >= 4) {   // a 400 ms window ends here: into the histogram, if above -70
        const double z = meanOf(4);
        const double lu = toLufs(z);
        if (lu > -70.0) {
            const int bin = std::min(kBins - 1, static_cast<int>((lu + 70.0) * 10.0));
            ++count_[bin];
            energy_[bin] += z;
            integrated_ = gated();
        }
        momentary_ = static_cast<float>(lu);
    }
    shortTerm_ = static_cast<float>(toLufs(meanOf(kShort)));
}

double Loudness::meanOf(int blocks) const {
    const int n = std::min(blocks, filled_);
    if (n == 0) return 0.0;
    double s = 0.0;
    for (int j = 1; j <= n; ++j) s += blocks_[(blockW_ - j + kShort) % kShort];
    return s / n;
}

float Loudness::gated() const {
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
