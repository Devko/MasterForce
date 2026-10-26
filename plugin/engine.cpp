#include "engine.h"

#include "master_map.h"

#include <algorithm>
#include <cmath>

namespace mf {

namespace {

constexpr int kGrHoldFrames = 11025;   // the reduction shown: the deepest of the last quarter second

int tenths(float db) { return static_cast<int>(std::lround(std::max(db, -99.0f) * 10.0f)); }

} // namespace

Engine::Engine() {
    float n[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) n[i] = PARAM_INFO[i].def;
    params_ = masterParams(n, 0.0f);
}

void Engine::setParams(const float* norm) {
    params_ = masterParams(norm, glueThr_);
}

void Engine::setLearnPresses(uint32_t presses) {
    // The surface's count starts at 0, as this one does. Presses that came while no block ran (the
    // slot off, suspended) count by their number: two are an arm and its cancel, nothing.
    const bool odd = ((presses - presses_) & 1u) != 0;
    presses_ = presses;
    if (!odd) return;
    const Master::LearnState s = master_.learnState();
    if (s == Master::LearnState::Armed || s == Master::LearnState::Measuring) {
        master_.cancelLearn();
    } else {
        master_.learn();
        posted_ = false;
    }
}

void Engine::reset() { master_.reset(); }

void Engine::render(float* L, float* R, int n, const ef::Transport& t) {
    params_.glueThresholdDb = glueThr_;
    const double beatsPerSample = t.bpm / 60.0 / ef::kRate;
    for (int pos = 0; pos < n; pos += ef::kChunk) {
        const int len = std::min(ef::kChunk, n - pos);
        ef::Transport c = t;
        if (t.playing && t.valid) c.beats = t.beats + pos * beatsPerSample;
        master_.set(params_, c);
        master_.process(L + pos, R + pos, len);
    }
    if (master_.learnState() == Master::LearnState::Done && !posted_) {
        const uint32_t next = mailSeq_.load(std::memory_order_relaxed) + 1;
        mail_[next & 1u] = master_.measurement();
        mailSeq_.store(next, std::memory_order_release);
        posted_ = true;
    }
    grHeld_ = std::max(grHeld_, master_.takeReductionDb());
    grFrames_ += n;
    if (grFrames_ >= kGrHoldFrames) {
        gr_.store(grHeld_, std::memory_order_relaxed);
        grHeld_ = 0.0f;
        grFrames_ = 0;
    }
    publish();
}

void Engine::publish() {
    const Loudness& m = master_.meter();
    const float st = m.shortTerm(), song = m.integrated(), tp = m.truePeakDb(), gr = gr_.load(std::memory_order_relaxed);
    short_.store(st, std::memory_order_relaxed);
    song_.store(song, std::memory_order_relaxed);
    peak_.store(tp, std::memory_order_relaxed);
    const int state = static_cast<int>(master_.learnState());
    const int bar = state == static_cast<int>(Master::LearnState::Measuring)
                        ? std::min(static_cast<int>(master_.learnBars()) + 1, kLearnBars)
                        : 0;
    learn_.store(state, std::memory_order_relaxed);
    learnBar_.store(bar, std::memory_order_relaxed);
    learnError_.store(master_.learnError(), std::memory_order_relaxed);
    uint32_t key = 0;
    for (const int v : {tenths(st), tenths(song), tenths(tp), tenths(gr), state, bar})
        key = key * 1000003u ^ static_cast<uint32_t>(v);
    metersKey_.store(key, std::memory_order_relaxed);
}

Engine::Meters Engine::meters() const {
    Meters m;
    m.shortTerm = short_.load(std::memory_order_relaxed);
    m.song = song_.load(std::memory_order_relaxed);
    m.truePeak = peak_.load(std::memory_order_relaxed);
    m.reduction = gr_.load(std::memory_order_relaxed);
    m.learn = learn_.load(std::memory_order_relaxed);
    m.learnBar = learnBar_.load(std::memory_order_relaxed);
    m.learnError = learnError_.load(std::memory_order_relaxed);
    return m;
}

bool Engine::takeMeasurement(Measurement& out, uint32_t& seen) const {
    const uint32_t seq = mailSeq_.load(std::memory_order_acquire);
    if (seq == seen) return false;
    out = mail_[seq & 1u];
    seen = seq;
    return true;
}

} // namespace mf
