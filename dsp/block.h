#pragma once
// MasterForce's per-block helpers on top of dsp/simd.h: a block's samples four at a time.
//
// Why: on ARMv7 a scalar float compare (std::max, a clamp, isfinite) moves the VFP's flags to the
// core and waits for its pipeline to drain first, so a loop of them per sample costs far more than
// its arithmetic. EffectForce's modules keep their per-sample work in vector lanes for that reason
// (dsp/comp.h); these do the same for the stages written here: compares become masks and selects.
#include "common.h"

#include <cfloat>
#include <cstring>

namespace mf {

// A NaN or infinity as 0, anything else as it is (a select, not arithmetic: even a denormal
// survives NEON's flush to zero): ef::sanitize() four at a time.
EF_INLINE ef::f4 sanitize4(ef::f4 x) {
#if EF_NEON
    return vbslq_f32(vcaleq_f32(x, vdupq_n_f32(FLT_MAX)), x, vdupq_n_f32(0.0f));   // |x| <= FLT_MAX: false for NaN
#else
    return (x >= ef::splat(-FLT_MAX)) & (x <= ef::splat(FLT_MAX)) ? x : ef::splat(0.0f);
#endif
}

EF_INLINE ef::f4 abs4(ef::f4 x) {
#if EF_NEON
    return vabsq_f32(x);
#else
    return ef::max4(x, -x);
#endif
}

// The same float, bit for bit: an integer compare (no FP flags), and a NaN is itself.
inline bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// The largest lane.
EF_INLINE float hmax4(ef::f4 x) {
#if EF_NEON
    float32x2_t h = vpmax_f32(vget_low_f32(x), vget_high_f32(x));
    return vget_lane_f32(vpmax_f32(h, h), 0);
#else
    return std::max(std::max(x[0], x[1]), std::max(x[2], x[3]));
#endif
}

// The smallest lane.
EF_INLINE float hmin4(ef::f4 x) {
#if EF_NEON
    float32x2_t h = vpmin_f32(vget_low_f32(x), vget_high_f32(x));
    return vget_lane_f32(vpmin_f32(h, h), 0);
#else
    return std::min(std::min(x[0], x[1]), std::min(x[2], x[3]));
#endif
}

// Copies of a few dozen floats, inline: a call into libc's memcpy costs more than these do. The
// build's -fno-tree-loop-distribute-patterns keeps GCC from turning them back into calls.
//   copy()      n floats, any n (`src` read no further than n)
//   copyDown()  N floats to a lower address in the same buffer (a shift: overlapping is fine), four
//               at a time, rounded up to whole fours (the buffer has the room)
inline void copy(float* dst, const float* src, int n) {
    int i = 0;
    for (; i + 4 <= n; i += 4) ef::store4(dst + i, ef::load4(src + i));
    for (; i < n; ++i) dst[i] = src[i];
}
template <int N>
EF_INLINE void copyDown(float* dst, const float* src) {
#pragma GCC unroll 16
    for (int i = 0; i < N; i += 4) ef::store4(dst + i, ef::load4(src + i));
}

// What a detector reads: sanitize4(), and anything past +-lim (+80 dBFS for TruePeak::kMaxIn: no
// signal) as +-lim, so no sum or square inside it can overflow.
EF_INLINE ef::f4 clampIn4(ef::f4 x, ef::f4 lim) { return ef::min4(ef::max4(sanitize4(x), -lim), lim); }

// ef::sanitize() over a buffer in place, four at a time (a last few one by one).
inline void sanitize(float* x, int n) {
    int i = 0;
    for (; i + 4 <= n; i += 4) ef::store4(x + i, sanitize4(ef::load4(x + i)));
    for (; i < n; ++i) x[i] = ef::sanitize(x[i]);
}

// clampIn4() over a buffer in place; `x` has room for n rounded up to a multiple of 4 (the lanes
// past n are cleaned too: they must hold something, and keep it finite).
inline void clampIn(float* x, int n, float lim) {
    const ef::f4 l = ef::splat(lim);
    for (int i = 0; i < n; i += 4) ef::store4(x + i, clampIn4(ef::load4(x + i), l));
}

// The largest |x| of a buffer (0 for none).
inline float peakOf(const float* x, int n) {
    ef::f4 m = ef::splat(0.0f);
    int i = 0;
    for (; i + 4 <= n; i += 4) m = ef::max4(m, abs4(ef::load4(x + i)));
    float p = hmax4(m);
    for (; i < n; ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}

} // namespace mf
