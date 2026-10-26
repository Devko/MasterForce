#include "limiter.h"
#include "svf.h"   // clampParam, f2

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mf {

namespace {

constexpr float kSnap = 1e-6f;   // a reduction this small is none
constexpr float kFastMs = 40.0f, kSlowMs = 300.0f;
constexpr float kDensitySeconds = 1.0f;

using ef::f2;

// x < y ? 0 : x, as a select.
EF_INLINE f2 zeroBelow(f2 x, f2 y) {
#if EF_NEON
    return vbsl_f32(vclt_f32(x, y), vdup_n_f32(0.0f), x);
#else
    return x < y ? ef::splat2(0.0f) : x;
#endif
}

} // namespace

Limiter::Limiter() { reset(); }

void Limiter::reset() {
    tp_.reset();
    lastInterval_ = 0.0f;
    reduction_ = 0.0f;
    density_ = 0.0f;
    std::fill(run_, run_ + kHold, 1.0f);
    std::fill(suffix_, suffix_ + kHold + 1, 1.0f);
    prefix_ = 1.0f;
    runPos_ = 0;
    std::fill(box_, box_ + kWindow, 1.0f);
    boxW_ = 0;
    sum_ = kWindow;
    for (float* d : delay_) std::fill(d, d + kLatency + kBlock + 4, 0.0f);
    minGain_ = 1.0f;
    densityCoef_ = ef::smoothCoef(kDensitySeconds);
    relDensity_ = 0.0f;
    relCoef_ = ef::smoothCoef(kFastMs * 0.001f);
    ceilDb_ = -1.0f;
    ceil_ = std::pow(10.0f, ceilDb_ / 20.0f);
}

void Limiter::set(const Params& p) {
    // libm only when something moved: the ceiling rarely, the density only while there are overs.
    const float db = ef::clampParam(p.ceilingDb, -12.0f, 0.0f, -1.0f);
    if (std::memcmp(&db, &ceilDb_, sizeof db) != 0) {
        ceilDb_ = db;
        ceil_ = std::pow(10.0f, db / 20.0f);
    }
    if (std::memcmp(&density_, &relDensity_, sizeof density_) != 0) {
        relDensity_ = density_;
        relCoef_ = ef::smoothCoef((kFastMs + (kSlowMs - kFastMs) * density_) * 0.001f);
    }
}

void Limiter::process(float* L, float* R, int n) {
    for (int pos = 0; pos < n; pos += kBlock) block(L + pos, R + pos, std::min(kBlock, n - pos));
}

void Limiter::block(float* L, float* R, int n) {
    const int groups = (n + 3) / 4;   // the lanes past n compute something unused

    // The audio into the delay line, sanitized.
    float* dl = delay_[0] + kLatency;
    float* dr = delay_[1] + kLatency;
    copy(dl, L, n);
    copy(dr, R, n);
    for (int g = 0; g < groups; ++g) {
        ef::store4(dl + 4 * g, sanitize4(ef::load4(dl + 4 * g)));
        ef::store4(dr + 4 * g, sanitize4(ef::load4(dr + 4 * g)));
    }

    // 1-2: the peak around each sample (its intervals on both sides) and the gain it needs:
    // ceil / max(peak, ceil) is ceil / peak over the ceiling and exactly 1 at or under it.
    alignas(16) float peaks[kBlock], need[kBlock], over[kBlock];
    const float last = tp_.process(dl, dr, n, peaks);
    const ef::f4 ceil4 = ef::splat(ceil_), one4 = ef::splat(1.0f), zero4 = ef::splat(0.0f);
    ef::f4 prev = ef::splat(lastInterval_);
    lastInterval_ = last;
    for (int g = 0; g < groups; ++g) {
        const ef::f4 cur = ef::load4(peaks + 4 * g);
        const ef::f4 peak = ef::max4(cur, ef::ext<3>(prev, cur));   // each interval and the one before it
        prev = cur;
        const ef::f4 nd = ceil4 / ef::max4(peak, ceil4);
        ef::store4(need + 4 * g, nd);
#if EF_NEON
        ef::store4(over + 4 * g, vbslq_f32(vcltq_f32(nd, one4), one4, zero4));
#else
        ef::store4(over + 4 * g, nd < one4 ? one4 : zero4);
#endif
    }

    // 3-5, sample by sample. The gains are never under the lowest need, and with the detector's
    // input clamped (TruePeak::kMaxIn) that is over 1e-5: every gain is a multiple of 2^-47, so
    // the window's sum in a double is exact. It never drifts, and with only 1s in the window it
    // is kWindow exactly and the gain 1.
    alignas(16) float gains[kBlock];
    const f2 keep = ef::splat2(1.0f - relCoef_), snap = ef::splat2(kSnap), one = ef::splat2(1.0f);
    f2 red = ef::splat2(reduction_), prefix = ef::splat2(prefix_);
    float density = density_;
    for (int i = 0; i < n; ++i) {
        // 3: release.
        red = ef::max2(zeroBelow(red * keep, snap), one - ef::splat2(need[i]));
        const f2 g = one - red;
        density += densityCoef_ * (over[i] - density);

        // 4: the running minimum over kHold.
        prefix = ef::min2(prefix, g);
        const f2 held2 = ef::min2(prefix, ef::splat2(suffix_[runPos_ + 1]));
#if EF_NEON
        vst1_lane_f32(run_ + runPos_, g, 0);
        const float held = vget_lane_f32(held2, 0);
#else
        run_[runPos_] = g[0];
        const float held = held2[0];
#endif
        if (++runPos_ == kHold) {   // a run complete: its suffix minima, for the next one's windows
            f2 m = one;
            for (int j = kHold - 1; j >= 0; --j) {
                m = ef::min2(m, ef::splat2(run_[j]));
#if EF_NEON
                vst1_lane_f32(suffix_ + j, m, 0);
#else
                suffix_[j] = m[0];
#endif
            }
            prefix = one;
            runPos_ = 0;
        }

        // 5: the moving average of the minimum.
        const float old = box_[boxW_];
        box_[boxW_] = held;
        boxW_ = boxW_ + 1 == kWindow ? 0 : boxW_ + 1;
        sum_ += static_cast<double>(held) - old;
        gains[i] = static_cast<float>(sum_ * (1.0 / kWindow));
    }
#if EF_NEON
    reduction_ = vget_lane_f32(red, 0);
    prefix_ = vget_lane_f32(prefix, 0);
#else
    reduction_ = red[0];
    prefix_ = prefix[0];
#endif
    density_ = density;

    // The audio, kLatency samples late, at that gain; the safety clip.
    const ef::f4 lo = -ceil4;
    ef::f4 least = one4;
    alignas(16) float outL[kBlock], outR[kBlock];
    for (int i = n; i < groups * 4; ++i) gains[i] = 1.0f;   // a group's lanes past the block
    for (int g = 0; g < groups; ++g) {
        const ef::f4 gain = ef::load4(gains + 4 * g);
        least = ef::min4(least, gain);
        ef::store4(outL + 4 * g, ef::min4(ef::max4(ef::load4(delay_[0] + 4 * g) * gain, lo), ceil4));   // x * 1.0f is x exactly
        ef::store4(outR + 4 * g, ef::min4(ef::max4(ef::load4(delay_[1] + 4 * g) * gain, lo), ceil4));
    }
    minGain_ = std::min(minGain_, hmin4(least));
    copy(L, outL, n);
    copy(R, outR, n);
    for (int c = 0; c < 2; ++c) copyDown<kLatency>(delay_[c], delay_[c] + n);
}

float Limiter::takeReductionDb() {
    const float g = minGain_;
    minGain_ = 1.0f;
    return g < 1.0f ? -20.0f * std::log10(std::max(g, 1e-6f)) : 0.0f;
}

} // namespace mf
