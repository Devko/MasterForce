// The chain: at rest the end of the chain and nothing more; LEARN on the grid; the whole flow.
#include "measure.h"

#include <string>

using namespace eft;
using mf::Master;

namespace {

double sectionLufs(const Buf& L, const Buf& R, size_t from, size_t to) {
    mf::Loudness m;
    m.process(L.data() + from, R.data() + from, static_cast<int>(to - from));
    return m.integrated();
}

} // namespace

void masterTests() {
    const double bpm = 120.0;
    const size_t bar = static_cast<size_t>(2.0 * ef::kRate);   // 4 beats at 120 BPM
    {   // Defaults: the input, kLatency late, bit for bit.
        Master m;
        const Buf inL = whiteNoise(static_cast<int>(ef::kRate), 0.3f, 21), inR = whiteNoise(static_cast<int>(ef::kRate), 0.3f, 22);
        Buf L = inL, R = inR;
        Master::Params p;
        runMaster(m, L, R, p);
        bool same = true;
        for (size_t i = Master::kLatency; i < L.size(); ++i)
            same = same && L[i] == inL[i - Master::kLatency] && R[i] == inR[i - Master::kLatency];
        CHECK(same);
    }
    {   // Hot, at the defaults: no sample over the ceiling, nothing but finite numbers.
        Master m;
        Buf L = whiteNoise(static_cast<int>(ef::kRate), 3.0f, 23), R = whiteNoise(static_cast<int>(ef::kRate), 3.0f, 24);
        Master::Params p;
        runMaster(m, L, R, p);
        CHECK(peak(L) <= 0.8913f && peak(R) <= 0.8913f);
        CHECK(allFinite(L) && allFinite(R));
    }
    {   // LEARN pressed on beat 2 of bar 1 starts on bar 2's line and measures 8 bars.
        Master m;
        Buf L, R;
        beat(L, R, static_cast<int>(12 * bar), 0.3f);
        const Buf inL = L, inR = R;
        Master::Params p;
        Master::LearnState atBar1 = Master::LearnState::Idle, atBar2 = Master::LearnState::Idle;
        runMaster(m, L, R, p, bpm, [&](size_t i, ef::Transport&, Master::Params&) {
            if (i == static_cast<size_t>(0.5 * ef::kRate) / ef::kChunk * ef::kChunk) m.learn();
            const size_t before = bar - 2 * ef::kChunk, after = bar + 2 * ef::kChunk;   // in the chunks round them
            if (i <= before && before < i + ef::kChunk) atBar1 = m.learnState();
            if (i <= after && after < i + ef::kChunk) atBar2 = m.learnState();
        });
        CHECK(atBar1 == Master::LearnState::Armed);
        CHECK(atBar2 == Master::LearnState::Measuring);
        CHECK(m.learnState() == Master::LearnState::Done);
        const mf::Measurement& me = m.measurement();
        CHECK(me.valid);
        CHECK(std::fabs(me.loudness - sectionLufs(inL, inR, bar, 9 * bar)) < 0.3);
    }
    {   // It fails if MPC stops while it measures, or the song position jumps; and it won't learn
        // silence. Armed while stopped, it waits for play.
        Master m;
        Buf L, R;
        beat(L, R, static_cast<int>(6 * bar), 0.3f);
        Master::Params p;
        runMaster(m, L, R, p, bpm, [&](size_t i, ef::Transport& t, Master::Params&) {
            if (i == 0) m.learn();
            if (i >= 3 * bar) t.playing = false;
        });
        CHECK(m.learnState() == Master::LearnState::Failed && std::string(m.learnError()) == "stopped");

        Master j;
        beat(L, R, static_cast<int>(6 * bar), 0.3f);
        runMaster(j, L, R, p, bpm, [&](size_t i, ef::Transport& t, Master::Params&) {
            if (i == 0) j.learn();
            if (i >= 3 * bar) t.beats += 16.0;
        });
        CHECK(j.learnState() == Master::LearnState::Failed && std::string(j.learnError()) == "moved");

        Master q;
        Buf zL(10 * bar, 0.0f), zR = zL;
        runMaster(q, zL, zR, p, bpm, [&](size_t i, ef::Transport&, Master::Params&) {
            if (i == 0) q.learn();
        });
        CHECK(q.learnState() == Master::LearnState::Failed && std::string(q.learnError()) == "too quiet");

        Master w;
        beat(L, R, static_cast<int>(2 * bar), 0.3f);
        runMaster(w, L, R, p, bpm, [&](size_t i, ef::Transport& t, Master::Params&) {
            if (i == 0) w.learn();
            t.playing = false;
        });
        CHECK(w.learnState() == Master::LearnState::Armed);
    }
    {   // The whole flow, as the prototype runs it: LEARN on bars 2-9, derive, the song through the
        // chain. The section comes out near its aim, the song's true peak under the ceiling.
        Buf L, R;
        beat(L, R, static_cast<int>(12 * bar), 0.6f);   // about -21 LUFS, as a mix reaches the master
        const Buf inL = L, inR = R;
        Master learner;
        Master::Params p;
        Buf aL = L, aR = R;
        runMaster(learner, aL, aR, p, bpm, [&](size_t i, ef::Transport&, Master::Params&) {
            if (i == 0) learner.learn();
        });
        CHECK(learner.learnState() == Master::LearnState::Done);
        const mf::Measurement& me = learner.measurement();
        const mf::Settings s = mf::derive(me, 0, 1, 1.0f);
        p.tone = s.tone;
        p.glueDb = s.glueDb;
        p.glueThresholdDb = mf::glueThresholdFor(me, s.glueDb);
        p.gainDb = s.gainDb;
        CHECK(s.gainDb > 3.0f);

        Master chain;
        L.resize(L.size() + Master::kLatency, 0.0f);
        R.resize(R.size() + Master::kLatency, 0.0f);
        runMaster(chain, L, R, p);
        L.erase(L.begin(), L.begin() + Master::kLatency);
        R.erase(R.begin(), R.begin() + Master::kLatency);
        const double out = sectionLufs(L, R, bar, 9 * bar), aim = mf::kTargets[0].lufs + mf::kAboveTarget;
        CHECK(std::fabs(out - aim) < 1.5);
        CHECK(dbOf(truePeakRef(L, R)) <= -1.0 + 0.1);
        CHECK(chain.meter().integrated() > -16.0f);
    }
    {   // Glue switched on and off while playing fades, no step: a steady tone's largest step stays
        // what the tone itself has.
        Master m;
        const int n = static_cast<int>(2 * ef::kRate);
        Buf L = sine(200.0, n, 0.3f), R = L;
        const float tone = maxStep(L);
        mf::Measurement me;
        me.valid = true;
        me.glueHist[static_cast<int>((-10.5f - mf::kHistLoDb) / mf::kHistStepDb)] = 1000;
        Master::Params p;
        runMaster(m, L, R, p, bpm, [&](size_t i, ef::Transport&, Master::Params& q) {
            const bool on = i >= static_cast<size_t>(n / 4) && i < static_cast<size_t>(3 * n / 4);
            q.glueDb = on ? 3.0f : 0.0f;
            q.glueThresholdDb = mf::glueThresholdFor(me, 3.0f);
        });
        CHECK(maxStep(L, Master::kLatency + 1) < tone * 1.3f);
    }
    {   // Warmth: a second harmonic and a little density, the level kept (within 0.5 dB at a mastered
        // level); at 0 none at all.
        const int n = static_cast<int>(ef::kRate);
        for (float w : {0.0f, 0.5f, 1.0f}) {
            Master m;
            Buf L = fadeIn(sine(1000.0, n, 0.3f)), R = L;
            const Buf in = L;
            Master::Params p;
            p.warmth = w;
            runMaster(m, L, R, p);
            const double h2 = db(magnitude(L, 2000.0, n / 2, n) / magnitude(L, 1000.0, n / 2, n));
            const double level = db(rms(L, n / 2, n) / rms(in, n / 2 - Master::kLatency, n - Master::kLatency));
            if (w == 0.0f) CHECK(h2 < -100.0);
            else CHECK(h2 > -50.0 && h2 < -20.0);
            CHECK(std::fabs(level) < 0.5);
        }
    }
    {   // Warmth switched on and off while playing glides: no step at the switches bigger than the
        // steady states either side have (saturation itself steepens a sine's waveform).
        Master m;
        const int n = static_cast<int>(2 * ef::kRate);
        Buf L = sine(200.0, n, 0.3f), R = L;
        Master::Params p;
        const size_t on = static_cast<size_t>(n / 4), off = static_cast<size_t>(3 * n / 4), lat = Master::kLatency;
        runMaster(m, L, R, p, bpm, [&](size_t i, ef::Transport&, Master::Params& q) { q.warmth = i >= on && i < off ? 1.0f : 0.0f; });
        const float steady = std::max(maxStep(L, lat + 1, on - 2000), maxStep(L, on + 8000, off - 2000));
        CHECK(maxStep(L, on + lat - 500, on + lat + 2000) < steady * 1.1f);
        CHECK(maxStep(L, off + lat - 500, off + lat + 2000) < steady * 1.1f);
    }
}
