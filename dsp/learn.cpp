#include "learn.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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

// The larger of |lane 0| and |lane 1|, in both.
EF_INLINE ef::f2 absMax2(ef::f2 v) {
#if EF_NEON
    const ef::f2 a = vabs_f32(v);
    return vpmax_f32(a, a);
#else
    const float m = std::max(std::fabs(v[0]), std::fabs(v[1]));
    return ef::f2{m, m};
#endif
}

EF_INLINE float lane0(ef::f2 v) {
#if EF_NEON
    return vget_lane_f32(v, 0);
#else
    return v[0];
#endif
}

// The response of a Tone setting at the bands.
void toneAtBands(const Tone::Params& p, float* out) {
    for (int b = 0; b < kBands; ++b) out[b] = Tone::responseDb(p, kBandHz[b]);
}

} // namespace

// --- measuring ---------------------------------------------------------------------------------

Learner::Learner() {
    float a1[kGroups * 4] = {}, ga1[kGroups * 4] = {}, gga1[kGroups * 4] = {};
    for (int b = 0; b < kGroups * 4; ++b) {   // the empty lanes: g 0, a band-pass of nothing
        const float g = b < kBands ? std::tan(ef::kPi * kBandHz[b] / ef::kRate) : 0.0f;
        a1[b] = 1.0f / (1.0f + g * (g + kOctaveK));
        ga1[b] = g * a1[b];
        gga1[b] = g * g * a1[b];
    }
    for (int j = 0; j < kGroups; ++j) {
        a1_[j] = ef::load4(a1 + 4 * j);
        ga1_[j] = ef::load4(ga1 + 4 * j);
        gga1_[j] = ef::load4(gga1 + 4 * j);
    }
    const float scG = std::tan(ef::kPi * kGlueScHz / ef::kRate);
    const float scA1 = 1.0f / (1.0f + scG * (scG + kButterK));
    scA1_ = ef::splat2(scA1);
    scGa1_ = ef::splat2(scG * scA1);
    scGga1_ = ef::splat2(scG * scG * scA1);
    ca_ = ef::smoothCoef(kGlueAttackMs * 0.001f);
    cr_ = ef::smoothCoef(kGlueReleaseMs * 0.001f);
    begin();
}

void Learner::begin() {
    for (int j = 0; j < kGroups; ++j) ic1_[j] = ic2_[j] = ef::splat(0.0f);
    std::fill(acc_, acc_ + kBands, 0.0);
    n_ = 0;
    meter_.reset();
    sc1_ = sc2_ = ef::splat2(0.0f);
    peak_ = level_ = 0.0f;
    std::fill(hist_, hist_ + kHistBins, 0u);
}

void Learner::process(const float* L, const float* R, int n) {
    meter_.process(L, R, n);
    const ef::f4 octK = ef::splat(kOctaveK), half = ef::splat(0.5f);
    const ef::f2 butterK = ef::splat2(kButterK), tiny = ef::splat2(kTiny), zero = ef::splat2(0.0f);
    const ef::f2 cr = ef::splat2(cr_), ca = ef::splat2(ca_);
    ef::f2 peak = ef::splat2(peak_), level = ef::splat2(level_);
    // Locals: the compiler can't keep members in registers across the histogram's stores.
    ef::f4 ic1[kGroups], ic2[kGroups];
    for (int j = 0; j < kGroups; ++j) {
        ic1[j] = ic1_[j];
        ic2[j] = ic2_[j];
    }
    ef::f2 sc1 = sc1_, sc2 = sc2_;
    alignas(16) float l[TruePeak::kBlock], r[TruePeak::kBlock];
    for (int pos = 0; pos < n; pos += TruePeak::kBlock) {   // clamped as the detectors read it: nothing overflows
        const int len = std::min(TruePeak::kBlock, n - pos);
        copy(l, L + pos, len);
        copy(r, R + pos, len);
        for (int i = len; i < (len + 3) / 4 * 4; ++i) l[i] = r[i] = 0.0f;   // clampIn() reads whole fours
        clampIn(l, len, TruePeak::kMaxIn);
        clampIn(r, len, TruePeak::kMaxIn);
        ef::f4 sq[kGroups];   // the bands' squares, summed in float over the block, then into doubles
        for (int j = 0; j < kGroups; ++j) sq[j] = ef::splat(0.0f);
        for (int i = 0; i < len; ++i) {
            // The bands of the mid (band-pass: k v1, unity at the centre).
            const ef::f4 m = half * (ef::splat(l[i]) + ef::splat(r[i]));
            for (int j = 0; j < kGroups; ++j) {
                const ef::f4 v3 = m - ic2[j];
                const ef::f4 v1 = a1_[j] * ic1[j] + ga1_[j] * v3;
                const ef::f4 v2 = ic2[j] + ga1_[j] * ic1[j] + gga1_[j] * v3;
                ic1[j] = v1 + v1 - ic1[j];
                ic2[j] = v2 + v2 - ic2[j];
                const ef::f4 y = octK * v1;
                sq[j] += y * y;
            }

            // The glue detector, as Comp's: the sidechain's high-pass, the louder side's peak.
            const ef::f2 x = ef::load2(l + i, r + i);
            const ef::f2 v3 = x - sc2;
            const ef::f2 v1 = scA1_ * sc1 + scGa1_ * v3;
            const ef::f2 v2 = sc2 + scGa1_ * sc1 + scGga1_ * v3;
            sc1 = v1 + v1 - sc1;
            sc2 = v2 + v2 - sc2;
            const ef::f2 d = absMax2(x - butterK * v1 - v2);
            peak = ef::max2(d, peak + cr * (d - peak) - tiny);
            level = ef::max2(zero, level + ca * (peak - level) - tiny);
            // gainToDb() but without its compare: under 1e-10 it reads -200, so does the floor's log.
            const float db = 6.02059991328f * ef::log2Fast(lane0(ef::max2(level, ef::splat2(1e-10f))));
            const int bin = static_cast<int>((db - kHistLoDb) / kHistStepDb);
            ++hist_[std::min(std::max(bin, 0), kHistBins - 1)];
        }
        for (int j = 0; j < kGroups; ++j) {
            alignas(16) float s[4];
            ef::store4(s, sq[j]);
            for (int k = 0; k < 4 && 4 * j + k < kBands; ++k) acc_[4 * j + k] += s[k];
        }
    }
    peak_ = lane0(peak);
    level_ = lane0(level);
    sc1_ = sc1;
    sc2_ = sc2;
    n_ += static_cast<uint64_t>(n);
    for (int j = 0; j < kGroups; ++j) {   // a decayed band to exact silence, both states at once
#if EF_NEON
        const uint32x4_t quiet = vandq_u32(vcaltq_f32(ic1[j], vdupq_n_f32(kTiny)), vcaltq_f32(ic2[j], vdupq_n_f32(kTiny)));
        ic1_[j] = vbslq_f32(quiet, vdupq_n_f32(0.0f), ic1[j]);
        ic2_[j] = vbslq_f32(quiet, vdupq_n_f32(0.0f), ic2[j]);
#else
        const auto quiet = (abs4(ic1[j]) < ef::splat(kTiny)) & (abs4(ic2[j]) < ef::splat(kTiny));
        ic1_[j] = quiet ? ef::splat(0.0f) : ic1[j];
        ic2_[j] = quiet ? ef::splat(0.0f) : ic2[j];
#endif
    }
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
