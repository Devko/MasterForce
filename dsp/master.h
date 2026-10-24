#pragma once
// The chain (docs/DESIGN.md): Tone, Glue, Gain, Warmth, Width, Clipper, Limiter, in that order, and
// LEARN.
//
//   IN -> Tone -> Glue -> Gain -> Warmth -> Width / mono bass -> Clipper (2x) -> Limiter (true peak) -> OUT
//
// Every stage at rest passes its input through bit for bit and costs nothing (Tone flat, Glue off,
// Gain 0 dB, Warmth 0, width 1 without mono bass, the clipper off while nothing drives it), and the
// limiter below its ceiling is a delay: by default the chain is the end of the chain and nothing more.
//
// LEARN: learn() arms it; it starts measuring on the next bar line while MPC plays and measures the
// input for kLearnBars bars. Then measurement() holds what it found: the caller (the plugin's UI
// thread; the prototype) derives settings from it (dsp/learn.h) and sets them as parameters. Stopping
// or a jump in the song position fails it. The audio thread never sets a parameter.
//
// Per chunk, as EffectForce's modules: set() with MPC's transport, then process() of at most kChunk
// samples in place.
#include "clipper.h"
#include "comp.h"
#include "drive.h"
#include "learn.h"
#include "limiter.h"
#include "loudness.h"
#include "tone.h"
#include "width.h"

namespace mf {

class Master {
public:
    static constexpr int kLatency = Limiter::kLatency;
    static constexpr float kClipOverDb = 1.0f;   // the clipper's level over the ceiling
    static constexpr float kWarmDriveDb = 6.0f;  // WARMTH at 100%: Tube driven this hard, all wet

    struct Params {
        Tone::Params tone;
        float glueDb = 0.0f;              // GLUE: 0 = off, else the mean reduction it was set for
        float glueThresholdDb = 0.0f;     // worked out from GLUE and the measurement (glueThresholdFor)
        float gainDb = 0.0f;              // -12..+18
        float warmth = 0.0f;              // WARMTH 0..1: 0 = off
        Width::Params width;
        float ceilingDb = -1.0f;          // dBTP
    };

    enum class LearnState { Idle, Armed, Measuring, Done, Failed };

    Master();
    void reset();
    void set(const Params& p, const ef::Transport& t);
    void process(float* L, float* R, int n);

    // LEARN.
    void learn();
    void cancelLearn();
    LearnState learnState() const { return learn_; }
    double learnBars() const { return learnBars_; }         // measured so far
    const char* learnError() const { return learnError_; }
    const Measurement& measurement() const { return measurement_; }

    // The meters, on the output.
    const Loudness& meter() const { return meter_; }
    void resetMeter() { meter_.reset(); }
    float takeReductionDb();   // the deepest reduction since the last call: clipper and limiter

private:
    Tone tone_;
    ef::Comp glue_;
    bool glueOn_ = false, glueOffNext_ = false, wasPlaying_ = false;
    int glueWarm_ = 0;
    float glueMix_ = 0.0f;
    ef::Comp::Params glueLast_;
    ef::Ramp gain_;
    ef::Drive warmth_;
    bool warmOn_ = false, warmOffNext_ = false;
    float warmMix_ = 0.0f;
    static constexpr int kWarmHistory = 256;   // a power of two, a whole number of chunks
    float warmHist_[2][kWarmHistory] = {};
    int warmHistPos_ = 0;
    ef::Drive::Params warmLast_;
    Width width_;
    Clipper clip_;
    Limiter limiter_;
    Loudness meter_;
    float clipPeak_ = 0.0f, clipLevel_ = 1.0f;

    Learner learner_;
    LearnState learn_ = LearnState::Idle;
    double learnStart_ = 0.0, lastBeats_ = 0.0, learnBars_ = 0.0;
    const char* learnError_ = "";
    Measurement measurement_;
    bool fresh_ = true;
};

} // namespace mf
