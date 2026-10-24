#pragma once
// True peak (ITU-R BS.1770-4, annex 2): the signal's peaks between its samples, found 4x
// oversampled. A full-scale signal can reach several dB over its highest sample between two of
// them, which a D/A converter or a lossy encoder then shows; the limiter keeps these under its
// ceiling, the meter reports them.
//
// The interpolator: a windowed sinc (Kaiser, beta 6), 24 taps per phase, 96 in all, each phase
// normalised to unity at DC: flat within 0.01 dB to 18 kHz, -1.3 dB at 20 kHz (the standard's own
// 12-tap filter falls 0.2 dB by 15 kHz and 2 dB by 18 kHz, and a limiter trusting it lets those
// peaks through). It reads 12 samples either side of the points it makes, so the interval that
// starts at sample m is known when sample m + 12 arrives: kDelay. Like any 4x measure it can also
// miss a pure tone's peak falling between its four points, by up to 0.6 dB near 20 kHz; for music,
// whose energy sits far lower, by a few hundredths.
#include "../dsp/common.h"

#include <cmath>

namespace mf {

class TruePeak {
public:
    static constexpr int kTaps = 24;            // per phase
    static constexpr int kDelay = kTaps / 2;    // the interval starting kDelay samples back is known

    TruePeak() {
        // h(t) = sinc(t) w(t / 12), for the points 1/4, 2/4, 3/4 of the way from sample m to m + 1,
        // from samples m - 11 .. m + 12. Tap i of a phase reads the history's sample i (oldest first).
        for (int p = 0; p < 3; ++p) {
            const double frac = (p + 1) * 0.25;
            double sum = 0.0;
            for (int i = 0; i < kTaps; ++i) {
                const double t = frac - (i - (kTaps / 2 - 1));   // distance from tap i's sample
                const double s = t == 0.0 ? 1.0 : std::sin(kPiD * t) / (kPiD * t);
                const double u = t / (kTaps / 2);
                const double w = u * u < 1.0 ? bessel0(6.0 * std::sqrt(1.0 - u * u)) / bessel0(6.0) : 0.0;
                taps_[p][i] = s * w;
                sum += s * w;
            }
            for (int i = 0; i < kTaps; ++i) taps_[p][i] /= sum;
        }
        reset();
    }

    void reset() {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 2 * kTaps; ++i) hist_[c][i] = 0.0f;
        w_ = 0;
    }

    // One stereo sample in; out: the peak (linear, both channels) of the interval from the sample
    // kDelay back to the one after it, both ends included.
    float push(float l, float r) {
        hist_[0][w_] = hist_[0][w_ + kTaps] = l;   // doubled, so a window is one contiguous read
        hist_[1][w_] = hist_[1][w_ + kTaps] = r;
        w_ = w_ + 1 == kTaps ? 0 : w_ + 1;
        float m = 0.0f;
        for (int c = 0; c < 2; ++c) {
            const float* h = hist_[c] + w_;   // oldest first: h[kTaps - 1] is the newest
            m = std::max(m, std::max(std::fabs(h[kDelay - 1]), std::fabs(h[kDelay])));
            for (int p = 0; p < 3; ++p) {
                float acc = 0.0f;
                for (int i = 0; i < kTaps; ++i) acc += taps_[p][i] * h[i];
                m = std::max(m, std::fabs(acc));
            }
        }
        return m;
    }

private:
    static constexpr double kPiD = 3.14159265358979323846;
    static double bessel0(double x) {   // I0 by its series; converges fast for x <= 6
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 32; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    float taps_[3][kTaps] = {};
    float hist_[2][2 * kTaps] = {};
    int w_ = 0;
};

} // namespace mf
