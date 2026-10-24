#pragma once
// The engine: MasterForce's chain (dsp/master.h) played by the parameters, chunk by chunk, with
// LEARN's hand-offs between the threads.
//
//   in   the parameters' snapshot (setParams), the glue's threshold, LEARN's presses (a counter
//        the surface bumps: a change arms LEARN, or cancels it while it waits or measures)
//   out  the measurement (a two-slot mailbox: the audio thread writes the slot it isn't showing,
//        then publishes it; a LEARN takes 8 bars, so the UI thread has long copied the other one),
//        and the meters and LEARN's state (atomics)
//
// Audio thread: setParams, setGlueThreshold, setLearnPresses, render, reset. Any thread: meters(),
// takeMeasurement().
#include "../dsp/master.h"
#include "param_ids.h"

#include <atomic>
#include <cstdint>

namespace mf {

class Engine {
public:
    Engine();

    void setParams(const float* norm);         // norm[P_COUNT]
    void setGlueThreshold(float db) { glueThr_ = db; }
    void setLearnPresses(uint32_t presses);
    // A block in place (any length): chunk by chunk, MPC's transport moved along it while it plays.
    void render(float* L, float* R, int n, const ef::Transport& t);
    void reset();                              // every tail cleared (a long suspend)
    int tailSamples() const { return Master::kLatency + 512; }

    // The newest measurement, if it is newer than `seen` (which it updates). UI thread.
    bool takeMeasurement(Measurement& out, uint32_t& seen) const;

    struct Meters {
        float shortTerm = Loudness::kSilence;   // LUFS, 3 s
        float song = Loudness::kSilence;        // LUFS, since MPC started playing
        float truePeak = Loudness::kSilence;    // dBTP, the highest since then
        float reduction = 0.0f;                 // dB: the deepest of the last quarter second
        int learn = 0;                          // Master::LearnState
        int learnBar = 0;                       // the bar it measures, 1..8 (Measuring)
        const char* learnError = "";
    };
    Meters meters() const;
    // What the meters' line shows, as a number: it changed, so MPC should redraw the texts.
    uint32_t metersKey() const { return metersKey_.load(std::memory_order_relaxed); }

private:
    void publish();

    Master master_;
    Master::Params params_;
    float glueThr_ = 0.0f;
    uint32_t presses_ = 0;

    Measurement mail_[2];
    std::atomic<uint32_t> mailSeq_{0};
    bool posted_ = false;   // this LEARN's measurement is in the mailbox

    std::atomic<float> short_{Loudness::kSilence}, song_{Loudness::kSilence}, peak_{Loudness::kSilence}, gr_{0.0f};
    std::atomic<int> learn_{0}, learnBar_{0};
    std::atomic<const char*> learnError_{""};
    std::atomic<uint32_t> metersKey_{0};
    float grHeld_ = 0.0f;
    int grFrames_ = 0;
};

} // namespace mf
