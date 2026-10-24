#pragma once
// Width and mono bass, on mid and side: M = (L + R) / 2, S = (L - R) / 2.
//
//   Width      the side's level, 0 (mono) .. 1.5
//   Mono bass  the side through a 4th-order high-pass (two Butterworth sections, Linkwitz-Riley's
//              slope) at 80, 120 or 160 Hz: the low end in the middle, where a phone speaker, a club
//              and a vinyl cut all want it
//
// At width 1 with mono bass off, once its ramps have landed, it isn't processed at all: the input
// passes bit for bit ((L + R) / 2 + (L - R) / 2 is L only within rounding). Width glides across a
// chunk; mono bass fades in or out across one, its filter starting from silence.
#include "../dsp/svf.h"   // common.h, clampParam

namespace mf {

class Width {
public:
    struct Params {
        float width = 1.0f;     // 0..1.5
        float monoHz = 0.0f;    // 0 = off, else 80..160
    };

    void reset() {
        clearFilter();
        width_.jump(1.0f);
        mono_.jump(0.0f);
        fresh_ = true;
    }

    void set(const Params& p, const ef::Transport&) {
        const float w = ef::clampParam(p.width, 0.0f, 1.5f, 1.0f);
        const bool on = p.monoHz >= 20.0f;
        if (on) {
            const float hz = ef::clampParam(p.monoHz, 20.0f, 400.0f, 120.0f);
            if (hz != hz_) {
                hz_ = hz;
                g_ = std::tan(ef::kPi * hz / ef::kRate);
                a1_ = 1.0f / (1.0f + g_ * (g_ + kButterK));
            }
            if (mono_.target() == 0.0f && mono_.value() == 0.0f) clearFilter();   // from off: from silence
        }
        if (fresh_) {
            width_.jump(w);
            mono_.jump(on ? 1.0f : 0.0f);
            fresh_ = false;
        } else {
            width_.to(w, ef::kChunk);
            mono_.to(on ? 1.0f : 0.0f, ef::kChunk);
        }
    }

    bool idle() const {
        return width_.value() == 1.0f && !width_.moving() && mono_.value() == 0.0f && !mono_.moving();
    }

    void process(float* L, float* R, int n) {
        if (idle()) return;
        for (int i = 0; i < n; ++i) {
            const float l = ef::sanitize(L[i]), r = ef::sanitize(R[i]);
            const float m = 0.5f * (l + r);
            float s = 0.5f * (l - r);
            const float mono = mono_.next();
            if (mono > 0.0f || mono_.moving()) s += mono * (highpass(s) - s);
            s *= width_.next();
            L[i] = m + s;
            R[i] = m - s;
        }
        flushTiny();
    }

private:
    static constexpr float kButterK = 1.41421356f;

    struct Section {   // a TPT state-variable high-pass (Zavalishin, Simper's form)
        float ic1 = 0.0f, ic2 = 0.0f;
    };

    float tick(Section& z, float x) const {
        const float v3 = x - z.ic2;
        const float v1 = a1_ * z.ic1 + g_ * a1_ * v3;
        const float v2 = z.ic2 + g_ * a1_ * z.ic1 + g_ * g_ * a1_ * v3;
        z.ic1 = 2.0f * v1 - z.ic1;
        z.ic2 = 2.0f * v2 - z.ic2;
        return x - kButterK * v1 - v2;
    }
    float highpass(float s) { return tick(hp_[1], tick(hp_[0], s)); }

    void clearFilter() { hp_[0] = hp_[1] = Section{}; }
    void flushTiny() {
        for (Section& z : hp_) {
            if (std::fabs(z.ic1) < 1e-20f) z.ic1 = 0.0f;
            if (std::fabs(z.ic2) < 1e-20f) z.ic2 = 0.0f;
        }
    }

    Section hp_[2];
    float hz_ = 0.0f, g_ = 0.0f, a1_ = 1.0f;
    ef::Ramp width_, mono_;
    bool fresh_ = true;
};

} // namespace mf
