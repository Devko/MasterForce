#include "limiter.h"
#include "svf.h"   // clampParam

#include <algorithm>
#include <cmath>

namespace mf {

namespace {

constexpr float kSnap = 1e-6f;   // a reduction this small is none
constexpr float kFastMs = 40.0f, kSlowMs = 300.0f;
constexpr float kDensitySeconds = 1.0f;

} // namespace

Limiter::Limiter() { reset(); }

void Limiter::reset() {
    tp_.reset();
    lastInterval_ = 0.0f;
    reduction_ = 0.0f;
    density_ = 0.0f;
    qHead_ = qLen_ = 0;
    t_ = 0;
    std::fill(box_, box_ + kRing, 1.0f);
    boxW_ = 0;
    sum_ = kWindow;
    notOne_ = 0;
    std::fill(dl_, dl_ + kDelayRing, 0.0f);
    std::fill(dr_, dr_ + kDelayRing, 0.0f);
    dw_ = 0;
    minGain_ = 1.0f;
    densityCoef_ = ef::smoothCoef(kDensitySeconds);
    relCoef_ = ef::smoothCoef(kFastMs * 0.001f);
}

void Limiter::set(const Params& p) {
    const float db = ef::clampParam(p.ceilingDb, -12.0f, 0.0f, -1.0f);
    ceil_ = std::pow(10.0f, db / 20.0f);
    relCoef_ = ef::smoothCoef((kFastMs + (kSlowMs - kFastMs) * density_) * 0.001f);
}

void Limiter::process(float* L, float* R, int n) {
    for (int i = 0; i < n; ++i) {
        const float l = ef::sanitize(L[i]), r = ef::sanitize(R[i]);

        // 1-2: the peak around the sample kDelay back, and the gain it needs.
        const float interval = tp_.push(l, r);
        const float peak = std::max(interval, lastInterval_);
        lastInterval_ = interval;
        const float need = peak > ceil_ ? ceil_ / peak : 1.0f;

        // 3: release.
        float red = reduction_ * (1.0f - relCoef_);
        if (red < kSnap) red = 0.0f;
        red = std::max(red, 1.0f - need);
        reduction_ = red;
        const float g = 1.0f - red;
        density_ += densityCoef_ * ((need < 1.0f ? 1.0f : 0.0f) - density_);

        // 4: the running minimum over kHold.
        ++t_;
        while (qLen_ > 0 && qv_[(qHead_ + qLen_ - 1) & kMask] >= g) --qLen_;
        qv_[(qHead_ + qLen_) & kMask] = g;
        qt_[(qHead_ + qLen_) & kMask] = t_;
        ++qLen_;
        while (t_ - qt_[qHead_] >= static_cast<uint32_t>(kHold)) {
            qHead_ = (qHead_ + 1) & kMask;
            --qLen_;
        }
        const float held = qv_[qHead_];

        // 5: the moving average of the minimum.
        const float old = box_[boxW_];
        box_[boxW_] = held;
        boxW_ = boxW_ + 1 == kWindow ? 0 : boxW_ + 1;
        notOne_ += (held != 1.0f) - (old != 1.0f);
        float gain;
        if (notOne_ == 0) {
            sum_ = kWindow;   // exact again: rounding left over from the last reduction goes
            gain = 1.0f;
        } else {
            sum_ += static_cast<double>(held) - old;
            gain = static_cast<float>(sum_ * (1.0 / kWindow));
        }
        minGain_ = std::min(minGain_, gain);

        // The audio, kLatency samples late, at that gain; the safety clip.
        dl_[dw_] = l;
        dr_[dw_] = r;
        const int rd = (dw_ - kLatency) & (kDelayRing - 1);
        dw_ = (dw_ + 1) & (kDelayRing - 1);
        L[i] = std::min(std::max(dl_[rd] * gain, -ceil_), ceil_);   // x * 1.0f is x exactly
        R[i] = std::min(std::max(dr_[rd] * gain, -ceil_), ceil_);
    }
}

float Limiter::takeReductionDb() {
    const float g = minGain_;
    minGain_ = 1.0f;
    return g < 1.0f ? -20.0f * std::log10(std::max(g, 1e-6f)) : 0.0f;
}

} // namespace mf
