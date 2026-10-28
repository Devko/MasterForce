#pragma once
// Loudness (ITU-R BS.1770-4, EBU R 128) in real time: what the page's line shows and what LEARN
// measures a section by.
//
// K-weighting (a high shelf and a high-pass, both channels, as libebur128 derives them; EffectWerk's
// tools/loudness.h has the same), summed into 100 ms blocks. It runs as two dsp/svf.h filters, L and
// R in the lanes of one vector: libebur128's biquads are the bilinear transforms of two analog
// sections prewarped at their corners, which is what an SVF with that g, k and mix is, and in float
// a state-variable filter stays exact where a direct form at 38 Hz would not. (The biquads remain,
// for weightingDb().) Momentary is the last 4 (400 ms),
// short-term the last 30 (3 s). Integrated is gated: every 400 ms window (one per 100 ms, 75%
// overlap) above -70 LUFS goes into a histogram of 0.1 dB bins, keeping each bin's count and summed
// energy; the relative gate (10 LU under the absolutely gated mean) and the mean above it are read
// from the bins. No allocation, any length (libebur128's histogram mode).
//
// The true peak is the highest since reset, dsp/truepeak.h on what was measured. The other
// readings change only when a 100 ms block ends, and are worked out then.
#include "truepeak.h"
#include "../dsp/svf.h"

#include <cstdint>

namespace mf {

class Loudness {
public:
    static constexpr float kSilence = -99.0f;   // what a reading of nothing shows

    Loudness();
    void reset();
    void process(const float* L, const float* R, int n);   // measures, doesn't touch the audio

    float momentary() const { return momentary_; }
    float shortTerm() const { return shortTerm_; }
    float integrated() const { return integrated_; }
    float truePeakDb() const;

    // The K-weighting's gain at a frequency, dB: what a band at that frequency counts for.
    static float weightingDb(float hz);

private:
    struct Biquad {
        double b0, b1, b2, a1, a2;
    };
    static constexpr int kBlock = 4410;     // 100 ms
    static constexpr int kShort = 30;       // blocks in 3 s
    static constexpr int kBins = 750;       // -70 .. +5 LUFS, 0.1 dB each

    static void design(Biquad& shelf, Biquad& high);
    void weight(const float* l, const float* r, int n);   // n <= TruePeak::kBlock
    void endBlock();
    double meanOf(int blocks) const;
    float gated() const;   // integrated loudness from the histogram

    ef::SvfUpdate shelfA_, highA_;
    ef::f2 shelfM_[3], highM_[3];
    ef::SvfState shelf_, high_;
    double acc_ = 0.0;
    int inBlock_ = 0;
    double blocks_[kShort] = {};   // mean squares, newest at blockW_ - 1
    int blockW_ = 0, filled_ = 0;
    uint32_t count_[kBins] = {};
    double energy_[kBins] = {};
    TruePeak tp_;
    float peak_ = 0.0f;
    float momentary_ = kSilence, shortTerm_ = kSilence, integrated_ = kSilence;
};

} // namespace mf
