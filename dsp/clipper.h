#pragma once
// Clipper: shaves the fastest peaks (a snare's crack, a kick's click) before the limiter, which then
// has less to do and pumps less. Modern loud masters clip first and limit after.
//
// The curve, on |x| with c the clip level and a knee of w = c / 5 either side of it: x itself up to
// c - w, a parabola from there to c + w (where it reaches c, flat), c beyond. Continuous with its
// slope throughout, so it adds harmonics gently as it bites. Run 2x oversampled (dsp/halfband.h, as
// EffectForce's Drive) so what it adds above 22 kHz doesn't fold back down as aliasing.
//
// It works only while the chain is driving (the gain above 0 dB, set by LEARN's target): otherwise
// it isn't processed at all and passes the input bit for bit. Switched, it crossfades over a chunk
// between the input and its 2x path (the halfbands' own delay, 2.7 samples at the low end, makes that
// one chunk a little phasey: it happens on LEARN or a change of target, not while playing).
#include "../dsp/halfband.h"
#include "../dsp/common.h"

#include <cstring>

namespace mf {

class Clipper {
public:
    void reset() {
        up_.reset();
        down_.reset();
        mix_.jump(0.0f);
        fresh_ = true;
    }

    // Once per chunk: the clip level (linear) and whether it works.
    void set(float level, bool on) {
        c_ = std::max(level, 1e-3f);
        if (on && mix_.target() == 0.0f && mix_.value() == 0.0f) {   // from off: from silence
            up_.reset();
            down_.reset();
        }
        if (fresh_) {
            mix_.jump(on ? 1.0f : 0.0f);
            fresh_ = false;
        } else {
            mix_.to(on ? 1.0f : 0.0f, ef::kChunk);
        }
    }

    bool idle() const { return mix_.value() == 0.0f && !mix_.moving(); }

    void process(float* L, float* R, int n) {
        if (idle()) return;
        const float w = 0.2f * c_;
        const ef::f4 a = ef::splat(c_ - w), b = ef::splat(c_ + w), span = ef::splat(2.0f * w), inv4w = ef::splat(1.0f / (4.0f * w)),
                     zero = ef::splat(0.0f);
        for (int i = 0; i < n; ++i) {
            const float l = ef::sanitize(L[i]), r = ef::sanitize(R[i]);
            const ef::f4 x = up_.process(l, r);   // (L early, L late, R early, R late)
            const ef::f4 ax = ef::max4(x, -x);
            const ef::f4 t = ef::min4(ef::max4(ax - a, zero), span);
            const ef::f4 y = copySign(ef::min4(ax, b) - t * t * inv4w, x);
            float cl, cr;
            down_.process(y, cl, cr);
            const float m = mix_.next();
            L[i] = l + m * (cl - l);
            R[i] = r + m * (cr - r);
        }
    }

private:
    static EF_INLINE ef::f4 copySign(ef::f4 mag, ef::f4 sign) {
#if EF_NEON
        return vbslq_f32(vdupq_n_u32(0x80000000u), sign, mag);
#else
        ef::i4 m, s;
        std::memcpy(&m, &mag, sizeof m);
        std::memcpy(&s, &sign, sizeof s);
        const ef::i4 out = (m & 0x7FFFFFFF) | (s & static_cast<int32_t>(0x80000000u));
        ef::f4 r;
        std::memcpy(&r, &out, sizeof r);
        return r;
#endif
    }

    ef::StereoInterpolator up_;
    ef::StereoDecimator down_;
    ef::Ramp mix_;
    float c_ = 1.0f;
    bool fresh_ = true;
};

} // namespace mf
