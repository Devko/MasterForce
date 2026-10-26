#pragma once
// Limiter: the chain's last stage, a stereo-linked look-ahead brickwall on the true peak.
//
//   1. Each sample's peak is the true peak (dsp/truepeak.h) of the intervals on both its sides, so
//      the points between it and its neighbours count as its own.
//   2. The gain it needs: ceiling / peak, or 1.
//   3. Release: the gain falls at once and comes back as a one-pole (40 ms, up to 300 ms while the
//      overs come thick: a dense passage doesn't pump, a lone hit lets go fast). Kept as the
//      reduction (1 - gain), which decays all the way: the gain itself would stall a few ulps under 1.
//   4. Held at its lowest over kHold samples (a running minimum): the averaging window and kExtend
//      either side of it. The extension matters for the true peak: a point between two samples is
//      rebuilt from several samples round it, so those must not sit higher on the gain's ramp.
//   5. Averaged over kWindow samples: a smooth fall that reaches the needed gain as the samples
//      kExtend before the peak leave the delay line, and holds it until kExtend after.
//
// So the audio is delayed by kLatency samples, 64 at 44.1 kHz (1.45 ms). MPC doesn't compensate
// latency, and the plugin reports none: on the master nothing runs beside it to be out of line.
//
// Below the ceiling the gain is exactly 1 and the output is the input, delayed, bit for bit: the
// reduction snaps to 0 under 1e-6, and the average is exactly 1 once its window holds only 1s.
// A hard clip at the ceiling after the gain is a safety for rounding; it never shapes the sound.
//
// Speed: a block at a time. Steps 1 and 2 and the output run four samples to a vector; the
// recursions of steps 3 to 5 run sample by sample, their compares as vector selects (dsp/block.h
// says why). The running minimum is van Herk and Gil-Werman's: the minima of the last whole run of
// kHold gains from each position to its end, and the minimum of the current run so far; the window
// is the least of the two. Two mins a sample and kHold every kHold samples, no branches on the data.
#include "truepeak.h"

namespace mf {

class Limiter {
public:
    static constexpr int kWindow = 45;                    // the gain's fall, samples (1 ms)
    static constexpr int kExtend = 8;                     // held this much either side of a peak
    static constexpr int kHold = kWindow + 2 * kExtend;
    static constexpr int kLatency = TruePeak::kDelay + kWindow - 1 + kExtend;   // 64

    struct Params {
        float ceilingDb = -1.0f;   // -12..0 dBTP
    };

    Limiter();
    void reset();
    void set(const Params& p);   // once per chunk
    void process(float* L, float* R, int n);

    // The deepest gain reduction (dB, >= 0) since the last call.
    float takeReductionDb();

private:
    static constexpr int kBlock = TruePeak::kBlock;
    void block(float* L, float* R, int n);   // n <= kBlock

    TruePeak tp_;
    float lastInterval_ = 0.0f;   // the previous interval's peak
    float ceil_ = 0.891f, ceilDb_ = -1.0f;
    float reduction_ = 0.0f, density_ = 0.0f, relCoef_ = 0.0f, densityCoef_ = 0.0f, relDensity_ = 0.0f;

    // The running minimum: the current run's gains, the suffix minima of the run before it (and 1
    // past its end), the current run's minimum so far.
    float run_[kHold] = {};
    float suffix_[kHold + 1] = {};
    float prefix_ = 1.0f;
    int runPos_ = 0;

    // The moving average.
    float box_[kWindow] = {};
    int boxW_ = 0;
    double sum_ = kWindow;

    // The audio's delay line: the last kLatency samples, then the block (and a group's room past it).
    alignas(16) float delay_[2][kLatency + kBlock + 4] = {};

    float minGain_ = 1.0f;
};

} // namespace mf
