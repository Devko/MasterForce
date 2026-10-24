#pragma once
// Measurements the tests judge the chain by, independent of the code under test: a true peak far
// finer than the chain's own (16x, a long windowed sinc, in double), program-like signals, and
// running a stage in chunks with MPC's transport.
#include "signal.h"
#include "../dsp/master.h"

#include <cmath>
#include <vector>

namespace eft {

// The true peak of a stereo pair, 16x oversampled with a 64-tap (each side) Kaiser-windowed sinc.
// Only where the filter has real samples on both sides: a signal cut off at a buffer's end overshoots
// there (silence assumed past it), a peak of the measurement, not of the signal.
inline double truePeakRef(const Buf& L, const Buf& R) {
    constexpr int kOver = 16, kSide = 32;
    static std::vector<double> h = [] {
        std::vector<double> t(static_cast<size_t>(kOver * 2 * kSide + 1));
        auto i0 = [](double x) {
            double s = 1.0, term = 1.0;
            for (int k = 1; k < 40; ++k) {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                s += term;
            }
            return s;
        };
        for (size_t i = 0; i < t.size(); ++i) {
            const double x = (static_cast<double>(i) - kOver * kSide) / kOver;   // in samples
            const double s = x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double u = x / kSide;
            t[i] = s * (u * u < 1.0 ? i0(10.0 * std::sqrt(1.0 - u * u)) / i0(10.0) : 0.0);
        }
        return t;
    }();
    double peak = 0.0;
    for (const Buf* x : {&L, &R}) {
        const int n = static_cast<int>(x->size());
        for (int m = 0; m < n; ++m) peak = std::max(peak, std::fabs(static_cast<double>((*x)[static_cast<size_t>(m)])));
        for (int m = kSide; m + kSide < n; ++m) {
            for (int p = 1; p < kOver; ++p) {
                double acc = 0.0;
                for (int j = -kSide + 1; j <= kSide; ++j)
                    acc += (*x)[static_cast<size_t>(m + j)] * h[static_cast<size_t>(kOver * kSide + p - kOver * j)];
                peak = std::max(peak, std::fabs(acc));
            }
        }
    }
    return peak;
}

// A fade-in over n samples (a raised cosine): a tone that starts abruptly overshoots between its
// first samples, a true peak of its own.
inline Buf fadeIn(Buf x, int n = 2048) {
    for (int i = 0; i < n && i < static_cast<int>(x.size()); ++i)
        x[static_cast<size_t>(i)] *= static_cast<float>(0.5 - 0.5 * std::cos(kPi * i / n));
    return x;
}

// White noise through two one-pole low-passes (12 dB an octave): its energy low, as music's is, and
// 40 dB down by 20 kHz (one pole would leave 20-22 kHz strong, where no 4x true-peak meter is exact).
inline Buf softNoise(int n, float amp, uint32_t seed, float hz = 2000.0f) {
    Buf x = whiteNoise(n, 1.0f, seed);
    const float c = 1.0f - std::exp(-2.0f * static_cast<float>(kPi) * hz / ef::kRate);
    float y1 = 0.0f, y = 0.0f, peak = 0.0f;
    for (float& s : x) {
        y1 += c * (s - y1);
        y += c * (y1 - y);
        s = y;
        peak = std::max(peak, std::fabs(y));
    }
    for (float& s : x) s *= amp / peak;
    return x;
}

// A beat at 120 BPM: a low thump on every beat over a bed of soft noise, peak about `amp`.
inline void beat(Buf& L, Buf& R, int n, float amp, uint32_t seed = 7) {
    L = softNoise(n, 0.25f * amp, seed);
    R = softNoise(n, 0.25f * amp, seed + 1);
    const int beatLen = static_cast<int>(ef::kRate * 0.5);
    for (int i = 0; i < n; ++i) {
        const int k = i % beatLen;
        const float env = std::exp(-static_cast<float>(k) / (0.06f * ef::kRate));
        const float thump = 0.75f * amp * env * static_cast<float>(std::sin(2.0 * kPi * 55.0 * k / ef::kRate));
        L[static_cast<size_t>(i)] += thump;
        R[static_cast<size_t>(i)] += thump;
    }
}

// The chain over a whole buffer in chunks, MPC playing from beat 0 at `bpm`. `each` runs before each
// chunk's set() with the chunk's first sample and the transport, and may change the params.
template <class Each>
void runMaster(mf::Master& m, Buf& L, Buf& R, mf::Master::Params& p, double bpm, Each each) {
    const size_t n = L.size();
    for (size_t i = 0; i < n; i += ef::kChunk) {
        const int len = static_cast<int>(std::min<size_t>(ef::kChunk, n - i));
        ef::Transport t;
        t.bpm = bpm;
        t.beats = static_cast<double>(i) / ef::kRate * bpm / 60.0;
        t.playing = t.valid = true;
        each(i, t, p);
        m.set(p, t);
        m.process(L.data() + i, R.data() + i, len);
    }
}
inline void runMaster(mf::Master& m, Buf& L, Buf& R, mf::Master::Params& p, double bpm = 120.0) {
    runMaster(m, L, R, p, bpm, [](size_t, ef::Transport&, mf::Master::Params&) {});
}

inline double dbOf(double v) { return 20.0 * std::log10(std::max(v, 1e-12)); }

} // namespace eft
