#pragma once
// Loudness (ITU-R BS.1770-4, EBU R 128) in real time: what the page's line shows and what LEARN
// measures a section by.
//
// K-weighting (a high shelf and a high-pass, both channels, as libebur128 derives them; EffectForce's
// tools/loudness.h has the same), summed into 100 ms blocks. Momentary is the last 4 (400 ms),
// short-term the last 30 (3 s). Integrated is gated: every 400 ms window (one per 100 ms, 75%
// overlap) above -70 LUFS goes into a histogram of 0.1 dB bins, keeping each bin's count and summed
// energy; the relative gate (10 LU under the absolutely gated mean) and the mean above it are read
// from the bins. No allocation, any length (libebur128's histogram mode).
//
// The true peak is the highest since reset, dsp/truepeak.h on what was measured.
#include "truepeak.h"

#include <cstdint>

namespace mf {

class Loudness {
public:
    static constexpr float kSilence = -99.0f;   // what a reading of nothing shows

    Loudness();
    void reset();
    void process(const float* L, const float* R, int n);   // measures, doesn't touch the audio

    float momentary() const;
    float shortTerm() const;
    float integrated() const;
    float truePeakDb() const;

    // The K-weighting's gain at a frequency, dB: what a band at that frequency counts for.
    static float weightingDb(float hz);

private:
    struct Biquad {
        double b0, b1, b2, a1, a2;
        double z1[2] = {}, z2[2] = {};
        double run(int c, double x) {
            const double y = b0 * x + z1[c];
            z1[c] = b1 * x - a1 * y + z2[c];
            z2[c] = b2 * x - a2 * y;
            return y;
        }
    };
    static constexpr int kBlock = 4410;     // 100 ms
    static constexpr int kShort = 30;       // blocks in 3 s
    static constexpr int kBins = 750;       // -70 .. +5 LUFS, 0.1 dB each

    static void design(Biquad& shelf, Biquad& high);
    double meanOf(int blocks) const;

    Biquad shelf_{}, high_{};
    double acc_ = 0.0;
    int inBlock_ = 0;
    double blocks_[kShort] = {};   // mean squares, newest at blockW_ - 1
    int blockW_ = 0, filled_ = 0;
    uint32_t count_[kBins] = {};
    double energy_[kBins] = {};
    TruePeak tp_;
    float peak_ = 0.0f;
};

} // namespace mf
