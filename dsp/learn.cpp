#include "learn.h"

#include <algorithm>
#include <cmath>

namespace mf {

namespace {

constexpr float kButterK = 1.41421356f;
constexpr float kOctaveK = 0.70710678f;   // 1 / Q for an octave-wide band-pass
constexpr float kTiny = 1e-20f;

float gainReduction(float levelDb, float thresholdDb) {
    const float slope = 1.0f - 1.0f / kGlueRatio, h = 0.5f * kGlueKneeDb;
    const float over = levelDb - thresholdDb;
    const float u = std::min(std::max(over + h, 0.0f), h + h);
    return slope * (u * u / (4.0f * h) + std::max(over - h, 0.0f));
}

// The response of a Tone setting at the bands.
void toneAtBands(const Tone::Params& p, float* out) {
    for (int b = 0; b < kBands; ++b) out[b] = Tone::responseDb(p, kBandHz[b]);
}

} // namespace

// --- measuring ---------------------------------------------------------------------------------

Learner::Learner() {
    for (int b = 0; b < kBands; ++b) {
        g_[b] = std::tan(ef::kPi * kBandHz[b] / ef::kRate);
        a1_[b] = 1.0f / (1.0f + g_[b] * (g_[b] + kOctaveK));
    }
    scG_ = std::tan(ef::kPi * kGlueScHz / ef::kRate);
    scA1_ = 1.0f / (1.0f + scG_ * (scG_ + kButterK));
    ca_ = ef::smoothCoef(kGlueAttackMs * 0.001f);
    cr_ = ef::smoothCoef(kGlueReleaseMs * 0.001f);
    begin();
}

void Learner::begin() {
    for (int b = 0; b < kBands; ++b) {
        band_[b] = Svf{};
        acc_[b] = 0.0;
    }
    n_ = 0;
    meter_.reset();
    sc_[0] = sc_[1] = Svf{};
    peak_ = level_ = 0.0f;
    std::fill(hist_, hist_ + kHistBins, 0u);
}

void Learner::process(const float* L, const float* R, int n) {
    meter_.process(L, R, n);
    for (int i = 0; i < n; ++i) {
        const float l = ef::sanitize(L[i]), r = ef::sanitize(R[i]);

        // The bands of the mid (band-pass: k v1, unity at the centre).
        const float m = 0.5f * (l + r);
        for (int b = 0; b < kBands; ++b) {
            Svf& z = band_[b];
            const float g = g_[b], a1 = a1_[b];
            const float v3 = m - z.ic2;
            const float v1 = a1 * z.ic1 + g * a1 * v3;
            const float v2 = z.ic2 + g * a1 * z.ic1 + g * g * a1 * v3;
            z.ic1 = 2.0f * v1 - z.ic1;
            z.ic2 = 2.0f * v2 - z.ic2;
            const float y = kOctaveK * v1;
            acc_[b] += static_cast<double>(y) * y;
        }

        // The glue detector, as Comp's: the sidechain's high-pass, the louder side's peak.
        float d = 0.0f;
        const float x[2] = {l, r};
        for (int c = 0; c < 2; ++c) {
            Svf& z = sc_[c];
            const float v3 = x[c] - z.ic2;
            const float v1 = scA1_ * z.ic1 + scG_ * scA1_ * v3;
            const float v2 = z.ic2 + scG_ * scA1_ * z.ic1 + scG_ * scG_ * scA1_ * v3;
            z.ic1 = 2.0f * v1 - z.ic1;
            z.ic2 = 2.0f * v2 - z.ic2;
            d = std::max(d, std::fabs(x[c] - kButterK * v1 - v2));
        }
        peak_ = std::max(d, peak_ + cr_ * (d - peak_) - kTiny);
        level_ = std::max(0.0f, level_ + ca_ * (peak_ - level_) - kTiny);
        const float db = ef::gainToDb(level_);
        const int bin = static_cast<int>((db - kHistLoDb) / kHistStepDb);
        ++hist_[std::min(std::max(bin, 0), kHistBins - 1)];
    }
    n_ += static_cast<uint64_t>(n);
    for (Svf& z : band_)
        if (std::fabs(z.ic1) < kTiny && std::fabs(z.ic2) < kTiny) z = Svf{};
}

Measurement Learner::finish() const {
    Measurement m;
    for (int b = 0; b < kBands; ++b) {
        const double ms = n_ ? acc_[b] / static_cast<double>(n_) : 0.0;
        m.bandDb[b] = ms > 1e-20 ? static_cast<float>(10.0 * std::log10(ms)) : -200.0f;
    }
    m.loudness = meter_.integrated();
    m.truePeakDb = meter_.truePeakDb();
    std::copy(hist_, hist_ + kHistBins, m.glueHist);
    m.valid = m.loudness > kMinLoudness;
    return m;
}

// --- what LEARN sets ---------------------------------------------------------------------------

BandFit fitBands(const float* bandDb) {
    // Least squares over the bands 125 Hz .. 8 kHz: x = -3 .. 3 octaves from 1 kHz, so sum(x) = 0.
    double sxy = 0.0, sy = 0.0, sxx = 0.0;
    for (int b = 2; b <= 8; ++b) {
        const double x = b - 5;
        sxy += x * bandDb[b];
        sy += bandDb[b];
        sxx += x * x;
    }
    BandFit f;
    f.slope = static_cast<float>(sxy / sxx);
    f.at1k = static_cast<float>(sy / 7.0);
    auto line = [&](int b) { return f.at1k + f.slope * static_cast<float>(b - 5); };
    f.subDev = 0.5f * ((bandDb[0] - line(0)) + (bandDb[1] - line(1)));
    f.airDev = bandDb[9] - line(9);
    return f;
}

// A section's peak-to-loudness ratio is its true peak over its gated loudness: a dense loop already
// squashed sits near 8 dB, a dynamic mix with sharp drums at 15-20. The target leaves its peaks
// `roomDb` (YouTube: the -1 dBTP ceiling over the section's -12.5 LUFS, 11.5 dB); what stands over
// that, the chain must take off somewhere. The rule:
//   - a little for cohesion: none at 8 dB (already dense, glue would only pump), 1 dB from 12 dB on
//   - and 30% of the excess: glue takes the slow part of the work, the clipper and the limiter the
//     fast part, so neither is pushed to pumping or crunching
//   - at most 4 dB: past that a 2:1 compressor is audibly working, not gluing
float glueAmountFor(float plrDb, float roomDb) {
    const float cohesion = std::min(std::max((plrDb - 8.0f) * 0.25f, 0.0f), 1.0f);
    const float excess = std::max(plrDb - roomDb, 0.0f);
    return std::min(cohesion + 0.3f * excess, 4.0f);
}

float glueReductionDb(const Measurement& m, float thresholdDb) {
    double sum = 0.0;
    uint64_t n = 0;
    for (int b = 0; b < kHistBins; ++b) {
        if (!m.glueHist[b]) continue;
        const float level = kHistLoDb + (static_cast<float>(b) + 0.5f) * kHistStepDb;
        sum += static_cast<double>(m.glueHist[b]) * gainReduction(level, thresholdDb);
        n += m.glueHist[b];
    }
    return n ? static_cast<float>(sum / static_cast<double>(n)) : 0.0f;
}

float glueThresholdFor(const Measurement& m, float amountDb) {
    // The reduction falls as the threshold rises: bisect between Comp's limits.
    float lo = -60.0f, hi = 0.0f;
    if (glueReductionDb(m, lo) <= amountDb) return lo;
    if (glueReductionDb(m, hi) >= amountDb) return hi;
    for (int i = 0; i < 24; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (glueReductionDb(m, mid) > amountDb) lo = mid;
        else hi = mid;
    }
    return 0.5f * (lo + hi);
}

Settings derive(const Measurement& m, int target, int style, float amount, float ceilingDb) {
    Settings s;
    if (!m.valid) return s;
    const StyleCurve& sc = kStyles[std::min(std::max(style, 0), kNumStyles - 1)];
    const TargetSpec& tg = kTargets[std::min(std::max(target, 0), kNumTargets - 1)];
    amount = std::min(std::max(amount, 0.0f), 1.0f);
    auto clampMove = [](float v) { return std::min(std::max(v, -kMaxMove), kMaxMove); };

    // Tilt: the slope the style wants over the slope measured, by what one dB of Tilt does to it.
    static const float tiltPerDb = [] {
        Tone::Params one;
        one.tiltDb = 1.0f;
        float r[kBands];
        toneAtBands(one, r);
        return fitBands(r).slope;
    }();
    const BandFit fit = fitBands(m.bandDb);
    s.tone.tiltDb = clampMove(amount * (sc.slope - fit.slope) / tiltPerDb);

    // Low and Air: the sub and air bands against the line, after the tilt.
    float bands[kBands], r[kBands];
    toneAtBands(s.tone, r);
    for (int b = 0; b < kBands; ++b) bands[b] = m.bandDb[b] + r[b];
    const BandFit tilted = fitBands(bands);
    static const float lowPerDb = [] {
        Tone::Params one;
        one.lowDb = 1.0f;
        return 0.5f * (Tone::responseDb(one, kBandHz[0]) + Tone::responseDb(one, kBandHz[1]));
    }();
    static const float airPerDb = [] {
        Tone::Params one;
        one.airDb = 1.0f;
        return Tone::responseDb(one, kBandHz[9]);
    }();
    s.tone.lowDb = clampMove(amount * (sc.subDb - tilted.subDev) / lowPerDb);
    s.tone.airDb = clampMove(amount * (sc.airDb - tilted.airDev) / airPerDb);

    const float room = tg.drive ? ceilingDb - (tg.lufs + kAboveTarget) : 99.0f;
    s.glueDb = std::min(std::max(amount * glueAmountFor(m.plr(), room), 0.0f), kGlueMaxDb);

    // Gain: to the target, over the section's loudness as Tone leaves it (estimated from the bands,
    // each by what the K-weighting counts it for). Glue's makeup keeps its level.
    if (tg.drive) {
        toneAtBands(s.tone, r);
        double before = 0.0, after = 0.0;
        for (int b = 0; b < kBands; ++b) {
            const double e = std::pow(10.0, (m.bandDb[b] + Loudness::weightingDb(kBandHz[b])) / 10.0);
            before += e;
            after += e * std::pow(10.0, r[b] / 10.0);
        }
        const float toneDb = before > 0.0 ? static_cast<float>(10.0 * std::log10(after / before)) : 0.0f;
        const float want = tg.lufs + kAboveTarget - (m.loudness + toneDb);
        s.gainDb = std::min(std::max(want, kMinGainDb), kMaxGainDb);
        s.shortDb = std::max(want - kMaxGainDb, 0.0f);
    }
    return s;
}

} // namespace mf
