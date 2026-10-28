#pragma once
// LEARN (docs/DESIGN.md, "What LEARN computes"): measure 8 bars of the loudest section, then set the
// chain's knobs from what was measured.
//
// Learner measures, on the audio thread and only while learning:
//   - octave bands of the mid, 31.25 Hz .. 16 kHz (state-variable band-passes, Q 1.41: an octave),
//     their mean squares in dB
//   - the section's gated loudness and its highest true peak (dsp/loudness.h)
//   - a histogram of the glue compressor's detector level, the detector rebuilt here exactly
//     (EffectWerk's Comp: the louder channel's peak after the sidechain low cut, an instant attack
//     with the release, then the attack): from it the mean gain reduction of any threshold follows,
//     so GLUE (an amount of reduction) becomes a threshold without playing the section again
//
// derive() turns a Measurement into settings, for a target, a style and an amount. It is a pure
// function: the plugin calls it again whenever TARGET, STYLE or AMOUNT moves, from the measurement
// kept in its state.
#include "loudness.h"
#include "tone.h"
#include "../dsp/svf.h"   // f2

#include <cstdint>

namespace mf {

constexpr int kLearnBars = 8;
constexpr double kBeatsPerBar = 4.0;
constexpr float kMinLoudness = -40.0f;   // a section quieter than this can't be judged
constexpr float kMaxMove = 3.0f;         // LEARN moves Tilt, Low and Air at most this far (dB)
constexpr float kAboveTarget = 1.5f;     // the section is aimed this far over the song's target

constexpr int kBands = 10;
inline constexpr float kBandHz[kBands] = {31.25f, 62.5f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};

// The glue compressor's settings, shared by the chain (EffectWerk's Comp) and the Learner's copy of
// its detector.
constexpr float kGlueRatio = 2.0f, kGlueKneeDb = 6.0f, kGlueAttackMs = 30.0f, kGlueReleaseMs = 300.0f, kGlueScHz = 100.0f;
constexpr float kGlueMaxDb = 6.0f;   // GLUE's range: 0 (off) .. this much mean reduction

constexpr float kHistLoDb = -60.0f, kHistStepDb = 0.5f;
constexpr int kHistBins = 132;   // -60 .. +6 dB

struct Measurement {
    bool valid = false;               // loud enough to judge
    float bandDb[kBands] = {};
    float loudness = Loudness::kSilence;     // LUFS, gated
    float truePeakDb = Loudness::kSilence;   // dBTP
    uint32_t glueHist[kHistBins] = {};

    float plr() const { return truePeakDb - loudness; }   // peak to loudness: how dynamic it is
};

class Learner {
public:
    Learner();
    void begin();
    void process(const float* L, const float* R, int n);
    Measurement finish() const;

private:
    // The bands side by side in the lanes of three f4s (the last two lanes empty: g 0, silent).
    static constexpr int kGroups = (kBands + 3) / 4;
    ef::f4 a1_[kGroups], ga1_[kGroups], gga1_[kGroups];   // a1, g a1, g g a1
    ef::f4 ic1_[kGroups], ic2_[kGroups];
    double acc_[kBands] = {};
    uint64_t n_ = 0;
    Loudness meter_;

    // The glue detector's sidechain high-pass, L and R in two lanes.
    ef::f2 scA1_, scGa1_, scGga1_;
    ef::f2 sc1_, sc2_;
    float ca_ = 0.0f, cr_ = 0.0f, peak_ = 0.0f, level_ = 0.0f;
    uint32_t hist_[kHistBins] = {};
};

// --- what LEARN sets ---------------------------------------------------------------------------

struct StyleCurve {
    const char* name;
    float slope;   // dB per octave, the octave bands from 125 Hz to 8 kHz
    float subDb;   // the 31 and 63 Hz bands over that line
    float airDb;   // the 16 kHz band over it
};
// Octave bands read pink noise flat (each octave holds as much of it as the next). A balanced
// commercial mix falls about 4.5 dB an octave per hertz, which is why spectrum analyzers tilt by 4.5
// (Voxengo SPAN's and FabFilter's default) to show one flat: 1.5 dB an octave steeper than pink, so
// -1.5 here. The sub sits about on that line (the 63 Hz octave on it, the 31 Hz one under), the 16 kHz
// octave a few dB under it (mixes fall away up there; lossy encoders cut it). Warm and Bright lean
// half a dB an octave either way, with the ends to match. `mfproto analyze` on references refines them.
inline constexpr StyleCurve kStyles[] = {
    {"Warm", -2.0f, 0.0f, -5.5f},
    {"Neutral", -1.5f, -1.0f, -4.0f},
    {"Bright", -1.0f, -2.0f, -2.5f},
};
constexpr int kNumStyles = 3;

struct TargetSpec {
    const char* name;
    float lufs;   // the song's integrated loudness
    bool drive;   // LEARN sets the gain (and the clipper works); Live: the limiter as a safety only
};
inline constexpr TargetSpec kTargets[] = {
    {"YouTube", -14.0f, true},
    {"Loud", -10.0f, true},
    {"Live", -14.0f, false},
};
constexpr int kNumTargets = 3;

// The bands' straight line (dB against octaves from 1 kHz, 125 Hz .. 8 kHz) and how far the sub and
// air bands sit from it.
struct BandFit {
    float slope = 0.0f;    // dB per octave
    float at1k = 0.0f;     // the line's level at 1 kHz
    float subDev = 0.0f;   // the 31 and 63 Hz bands' mean over the line there
    float airDev = 0.0f;   // the 16 kHz band over the line there
};
BandFit fitBands(const float* bandDb);

struct Settings {
    Tone::Params tone;
    float glueDb = 0.0f;   // GLUE: the mean reduction wanted, 0 = off
    float gainDb = 0.0f;   // into the clipper and the limiter
    float shortDb = 0.0f;  // how far the gain's range left the target unreached (a section too quiet)
};
constexpr float kMaxGainDb = 18.0f, kMinGainDb = -12.0f;
Settings derive(const Measurement& m, int target, int style, float amount, float ceilingDb = -1.0f);

// How much glue (mean gain reduction, dB) a section wants, by its peak-to-loudness ratio and the
// room the target leaves its peaks (the ceiling over the section's aim; plenty for Live).
float glueAmountFor(float plrDb, float roomDb);

// The glue compressor's mean gain reduction over the measured section at a threshold, and the
// threshold that gives a wanted mean reduction (makeup: that same amount, so GLUE keeps the level).
float glueReductionDb(const Measurement& m, float thresholdDb);
float glueThresholdFor(const Measurement& m, float amountDb);

} // namespace mf
