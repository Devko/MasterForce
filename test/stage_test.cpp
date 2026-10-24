// Tone, Width and the Clipper: each at rest passes its input bit for bit; each does what it says.
#include "measure.h"

using namespace eft;

namespace {

template <class Stage, class P>
void runStage(Stage& s, const P& p, Buf& L, Buf& R) {
    for (size_t i = 0; i < L.size(); i += ef::kChunk) {
        const int n = static_cast<int>(std::min<size_t>(ef::kChunk, L.size() - i));
        s.set(p, ef::Transport{});
        s.process(L.data() + i, R.data() + i, n);
    }
}

// The steady gain of a sine through Tone, dB.
double toneGainDb(const mf::Tone::Params& p, double hz) {
    mf::Tone t;
    t.reset();
    const int n = 32768;
    Buf L = sine(hz, n, 0.25f), R = L;
    const Buf in = L;
    runStage(t, p, L, R);
    return db(rms(L, n / 2, n) / rms(in, n / 2, n));
}

} // namespace

void stageTests() {
    const int n = 16384;
    {   // Tone flat: bit for bit.
        mf::Tone t;
        t.reset();
        const Buf in = whiteNoise(n, 0.5f, 11);
        Buf L = in, R = in;
        runStage(t, mf::Tone::Params{}, L, R);
        CHECK(L == in && R == in);
    }
    {   // responseDb(), LEARN's estimate, is what the EQs do (within 0.15 dB to 12 kHz).
        mf::Tone::Params p;
        p.tiltDb = 4.0f;
        p.lowDb = -3.0f;
        p.airDb = 2.0f;
        for (double hz : {40.0, 100.0, 250.0, 1000.0, 3000.0, 8000.0, 12000.0}) {
            const double got = toneGainDb(p, hz), want = mf::Tone::responseDb(p, static_cast<float>(hz));
            CHECK(std::fabs(got - want) < 0.15);
        }
        // Tilt is a tilt: the bottom down by half, the top up by half, about 0 round 1 kHz.
        mf::Tone::Params t;
        t.tiltDb = 4.0f;
        CHECK(std::fabs(mf::Tone::responseDb(t, 30.0f) + 2.0f) < 0.2f);
        CHECK(std::fabs(mf::Tone::responseDb(t, 18000.0f) - 2.0f) < 0.2f);
        CHECK(std::fabs(mf::Tone::responseDb(t, 1000.0f)) < 0.5f);
    }
    {   // Width at 1 without mono bass: bit for bit. Width 0: mono.
        mf::Width w;
        w.reset();
        const Buf inL = whiteNoise(n, 0.5f, 12), inR = whiteNoise(n, 0.5f, 13);
        Buf L = inL, R = inR;
        runStage(w, mf::Width::Params{}, L, R);
        CHECK(L == inL && R == inR);

        mf::Width::Params mono;
        mono.width = 0.0f;
        runStage(w, mono, L, R);
        bool same = true;
        for (size_t i = 2 * ef::kChunk; i < L.size(); ++i) same = same && L[i] == R[i];
        CHECK(same);
    }
    {   // Mono bass at 120 Hz: a 40 Hz side goes (more than 20 dB down), a 1 kHz side stays.
        for (double hz : {40.0, 1000.0}) {
            mf::Width w;
            w.reset();
            mf::Width::Params p;
            p.monoHz = 120.0f;
            Buf L = sine(hz, n, 0.5f), R = L;
            for (float& s : R) s = -s;   // all side
            const Buf in = L;
            runStage(w, p, L, R);
            const double change = db(rms(L, n / 2, n) / rms(in, n / 2, n));
            if (hz < 100.0) CHECK(change < -20.0);
            else CHECK(std::fabs(change) < 0.1);
        }
    }
    {   // The clipper off: bit for bit. On, at 1.0: a hot sine comes out at about 1.0; a quiet one
        // (under the knee) passes at its level.
        mf::Clipper c;
        c.reset();
        const Buf in = sine(100.0, n, 2.0f);
        Buf L = in, R = in;
        for (size_t i = 0; i < L.size(); i += ef::kChunk) {
            c.set(1.0f, false);
            c.process(L.data() + i, R.data() + i, ef::kChunk);
        }
        CHECK(L == in);

        mf::Clipper on;
        on.reset();
        for (size_t i = 0; i < L.size(); i += ef::kChunk) {
            on.set(1.0f, true);
            on.process(L.data() + i, R.data() + i, ef::kChunk);
        }
        CHECK(peak(L, n / 2, n) < 1.03f && peak(L, n / 2, n) > 0.97f);

        mf::Clipper soft;
        soft.reset();
        Buf q = sine(100.0, n, 0.5f), qr = q;
        const Buf qin = q;
        for (size_t i = 0; i < q.size(); i += ef::kChunk) {
            soft.set(1.0f, true);
            soft.process(q.data() + i, qr.data() + i, ef::kChunk);
        }
        CHECK(std::fabs(db(rms(q, n / 2, n) / rms(qin, n / 2, n))) < 0.01);
    }
}
