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
//
// Speed: a block at a time, four consecutive samples' intervals in the lanes of one f4
// (dsp/simd.h), both channels and all three phases from one read of each tap; abs and max in the
// lanes too. Each lane sums its taps in order, as a scalar loop would, and nothing compares a
// scalar float (on ARMv7 that moves the VFP's flags to the core and waits for its pipeline).
#include "block.h"

#include <algorithm>
#include <cmath>

namespace mf {

class TruePeak {
public:
    static constexpr int kTaps = 24;            // per phase
    static constexpr int kDelay = kTaps / 2;    // the interval starting kDelay samples back is known
    static constexpr int kBlock = 32;           // process() takes at most this many samples, a multiple of 4
    static constexpr float kMaxIn = 1e4f;       // +80 dBFS: the detector reads anything past it as this

    TruePeak() {
        // h(t) = sinc(t) w(t / 12), for the points 1/4, 2/4, 3/4 of the way from sample m to m + 1,
        // from samples m - 11 .. m + 12. Tap i of a phase reads the window's sample i (oldest first).
        float taps[3][kTaps];
        for (int p = 0; p < 3; ++p) {
            const double frac = (p + 1) * 0.25;
            double sum = 0.0;
            for (int i = 0; i < kTaps; ++i) {
                const double t = frac - (i - (kTaps / 2 - 1));   // distance from tap i's sample
                const double s = t == 0.0 ? 1.0 : std::sin(kPiD * t) / (kPiD * t);
                const double u = t / (kTaps / 2);
                const double w = u * u < 1.0 ? bessel0(6.0 * std::sqrt(1.0 - u * u)) / bessel0(6.0) : 0.0;
                taps[p][i] = static_cast<float>(s * w);
                sum += s * w;
            }
            for (int i = 0; i < kTaps; ++i) taps[p][i] = static_cast<float>(taps[p][i] / sum);
        }
        for (int p = 0; p < 3; ++p)
            for (int q = 0; q < kTaps / 4; ++q) taps_[q][p] = ef::load4(taps[p] + 4 * q);
        reset();
    }

    void reset() {
        for (float* h : hist_) std::fill(h, h + kHist + kBlock + 4, 0.0f);
    }

    // n <= kBlock stereo samples in; out, peaks[i] (room for kBlock): the peak (linear, both
    // channels) of the interval from the sample kDelay before sample i to the one after it, both
    // ends included; returned, the last of them. The detector reads NaN and the infinities as 0 and
    // clamps the rest to +-kMaxIn, so no sum of its taps can overflow.
    float process(const float* L, const float* R, int n, float* peaks) {
        const ef::f4 zero = ef::splat(0.0f);
        const int groups = (n + 3) / 4;
        for (int c = 0; c < 2; ++c) {
            float* h = hist_[c] + kHist;
            copy(h, c ? R : L, n);
            clampIn(h, n, kMaxIn);   // the lanes past n hold older history: finite, unused
        }
        for (int g = 0; g < groups; ++g) {
            const float* l = hist_[0] + 4 * g;   // lane j: the window of sample 4g + j, oldest first
            const float* r = hist_[1] + 4 * g;
            ef::f4 l0 = zero, l1 = zero, l2 = zero, r0 = zero, r1 = zero, r2 = zero;
            // The windows' sample k, four lanes at a time, from two aligned fours: ext<k % 4>(a, b).
            // (ARMv7's vector loads take no offset: one sliding load per tap costs an add each.)
            ef::f4 la = ef::load4(l), ra = ef::load4(r);
#pragma GCC unroll 1   // unrolled, GCC hoists all 18 tap vectors out of the group loop and spills them
            for (int q = 0; q < kTaps / 4; ++q) {
                const ef::f4 lb = ef::load4(l + 4 * q + 4), rb = ef::load4(r + 4 * q + 4);
                const ef::f4* t = taps_[q];
                tap<0>(l0, l1, l2, la, t);
                tap<0>(r0, r1, r2, ra, t);
                tap<1>(l0, l1, l2, ef::ext<1>(la, lb), t);
                tap<1>(r0, r1, r2, ef::ext<1>(ra, rb), t);
                tap<2>(l0, l1, l2, ef::ext<2>(la, lb), t);
                tap<2>(r0, r1, r2, ef::ext<2>(ra, rb), t);
                tap<3>(l0, l1, l2, ef::ext<3>(la, lb), t);
                tap<3>(r0, r1, r2, ef::ext<3>(ra, rb), t);
                la = lb;
                ra = rb;
            }
            ef::f4 m = ef::max4(abs4(ef::load4(l + kDelay - 1)), abs4(ef::load4(l + kDelay)));   // the interval's ends
            m = ef::max4(m, ef::max4(abs4(ef::load4(r + kDelay - 1)), abs4(ef::load4(r + kDelay))));
            m = ef::max4(m, ef::max4(ef::max4(abs4(l0), abs4(l1)), abs4(l2)));
            m = ef::max4(m, ef::max4(ef::max4(abs4(r0), abs4(r1)), abs4(r2)));
            ef::store4(peaks + 4 * g, m);
        }
        for (int c = 0; c < 2; ++c) copyDown<kHist>(hist_[c], hist_[c] + n);   // the next block's past
        return peaks[n - 1];
    }

    // The highest of those peaks over any number of samples: what a meter keeps.
    float maxOf(const float* L, const float* R, int n) {
        ef::f4 m = ef::splat(0.0f);
        float peaks[kBlock];
        for (int pos = 0; pos < n; pos += kBlock) {
            const int len = std::min(kBlock, n - pos);
            process(L + pos, R + pos, len, peaks);
            for (int i = len; i < (len + 3) / 4 * 4; ++i) peaks[i] = 0.0f;   // a group's lanes past the block
            for (int i = 0; i < len; i += 4) m = ef::max4(m, ef::load4(peaks + i));
        }
        return hmax4(m);
    }

private:
    // Tap 4q + J of the three phases (t[0..2]: phase p's taps 4q .. 4q + 3) into their sums.
    template <int J>
    static EF_INLINE void tap(ef::f4& a0, ef::f4& a1, ef::f4& a2, ef::f4 x, const ef::f4* t) {
#if EF_NEON
        a0 = vmlaq_lane_f32(a0, x, J < 2 ? vget_low_f32(t[0]) : vget_high_f32(t[0]), J % 2);
        a1 = vmlaq_lane_f32(a1, x, J < 2 ? vget_low_f32(t[1]) : vget_high_f32(t[1]), J % 2);
        a2 = vmlaq_lane_f32(a2, x, J < 2 ? vget_low_f32(t[2]) : vget_high_f32(t[2]), J % 2);
#else
        a0 += t[0][J] * x;
        a1 += t[1][J] * x;
        a2 += t[2][J] * x;
#endif
    }

    static constexpr double kPiD = 3.14159265358979323846;
    static constexpr int kHist = kTaps - 1;   // the samples before a block its first window reaches back to
    static double bessel0(double x) {   // I0 by its series; converges fast for x <= 6
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 32; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    ef::f4 taps_[kTaps / 4][3];   // [q][p]: taps 4q .. 4q + 3 of phase (p + 1) / 4
    // Per channel: the last kHist samples, then the block; room past it for a group's last window.
    alignas(16) float hist_[2][kHist + kBlock + 4] = {};
};

} // namespace mf
