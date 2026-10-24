// LEARN: the measuring, the fit, the rules that turn a measurement into settings.
#include "measure.h"

using namespace eft;

namespace {

// A measurement whose bands lie on a line of `slope` (dB per octave from 1 kHz, at -30 dB there),
// with the sub and air bands `sub` and `air` over it.
mf::Measurement onLine(float slope, float sub, float air, float loudness = -20.0f) {
    mf::Measurement m;
    for (int b = 0; b < mf::kBands; ++b) m.bandDb[b] = -30.0f + slope * static_cast<float>(b - 5);
    m.bandDb[0] += sub;
    m.bandDb[1] += sub;
    m.bandDb[9] += air;
    m.loudness = loudness;
    m.truePeakDb = loudness + 12.0f;
    m.valid = true;
    return m;
}

} // namespace

void learnTests() {
    {   // The fit finds the line, and the sub and air over it.
        const mf::Measurement m = onLine(-4.0f, 3.0f, -2.0f);
        const mf::BandFit f = mf::fitBands(m.bandDb);
        CHECK(std::fabs(f.slope + 4.0f) < 1e-4f);
        CHECK(std::fabs(f.at1k + 30.0f) < 1e-4f);
        CHECK(std::fabs(f.subDev - 3.0f) < 1e-4f);
        CHECK(std::fabs(f.airDev + 2.0f) < 1e-4f);
    }
    {   // The Learner measures a slope: sines at the bands' centres falling 4 dB an octave.
        mf::Learner l;
        const int n = static_cast<int>(4 * ef::kRate);
        Buf x(static_cast<size_t>(n), 0.0f);
        for (int b = 0; b < mf::kBands; ++b) {
            const Buf s = sine(mf::kBandHz[b], n, 0.05f * std::pow(10.0f, -4.0f * static_cast<float>(b - 5) / 20.0f), b);
            for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] += s[static_cast<size_t>(i)];
        }
        l.process(x.data(), x.data(), n);
        const mf::Measurement m = l.finish();
        CHECK(m.valid);
        CHECK(std::fabs(mf::fitBands(m.bandDb).slope + 4.0f) < 0.5f);
        mf::Loudness ref;
        ref.process(x.data(), x.data(), n);
        CHECK(std::fabs(m.loudness - ref.integrated()) < 1e-3f);
    }
    {   // Silence is too quiet to learn.
        mf::Learner l;
        const Buf z(44100, 0.0f);
        l.process(z.data(), z.data(), 44100);
        CHECK(!l.finish().valid);
    }

    const int neutral = 1, youtube = 0, loud = 1, live = 2;
    const mf::StyleCurve& sc = mf::kStyles[neutral];
    {   // Already where the style wants it: no tone moves.
        const mf::Settings s = mf::derive(onLine(sc.slope, sc.subDb, sc.airDb), youtube, neutral, 1.0f);
        CHECK(std::fabs(s.tone.tiltDb) < 0.01f && std::fabs(s.tone.lowDb) < 0.01f && std::fabs(s.tone.airDb) < 0.01f);
    }
    {   // 0.3 dB an octave too bright: Tilt brings the slope back to the style's (within 0.05). (A whole
        // dB an octave is 6 dB from 125 Hz to 8 kHz: more than LEARN's 3 dB of Tilt moves.)
        const mf::Measurement m = onLine(sc.slope + 0.3f, sc.subDb, sc.airDb);
        const mf::Settings s = mf::derive(m, youtube, neutral, 1.0f);
        CHECK(s.tone.tiltDb < 0.0f);
        float after[mf::kBands];
        for (int b = 0; b < mf::kBands; ++b) after[b] = m.bandDb[b] + mf::Tone::responseDb(s.tone, mf::kBandHz[b]);
        CHECK(std::fabs(mf::fitBands(after).slope - sc.slope) < 0.05f);
        // Half the amount, about half the move; none, none.
        const mf::Settings half = mf::derive(m, youtube, neutral, 0.5f);
        CHECK(std::fabs(half.tone.tiltDb - 0.5f * s.tone.tiltDb) < 0.01f);
        const mf::Settings none = mf::derive(m, youtube, neutral, 0.0f);
        CHECK(none.tone.tiltDb == 0.0f && none.tone.lowDb == 0.0f && none.tone.airDb == 0.0f && none.glueDb == 0.0f);
    }
    {   // Far off: every move stops at kMaxMove.
        const mf::Settings s = mf::derive(onLine(sc.slope + 6.0f, sc.subDb - 10.0f, sc.airDb + 10.0f), youtube, neutral, 1.0f);
        CHECK(s.tone.tiltDb == -mf::kMaxMove);
        CHECK(s.tone.lowDb == mf::kMaxMove);
        CHECK(s.tone.airDb == -mf::kMaxMove);
    }
    {   // Gain: from -20 LUFS to YouTube's -14, aimed 1.5 dB over: +7.5; Loud +11.5; Live none.
        const mf::Measurement m = onLine(sc.slope, sc.subDb, sc.airDb, -20.0f);
        CHECK(std::fabs(mf::derive(m, youtube, neutral, 1.0f).gainDb - 7.5f) < 0.05f);
        CHECK(std::fabs(mf::derive(m, loud, neutral, 1.0f).gainDb - 11.5f) < 0.05f);
        CHECK(mf::derive(m, live, neutral, 1.0f).gainDb == 0.0f);
        // Tone moves that add loudness (boosts at both ends) take it off the gain.
        const mf::Measurement thin = onLine(sc.slope, sc.subDb - 2.0f, sc.airDb - 2.0f, -20.0f);
        const mf::Settings s = mf::derive(thin, youtube, neutral, 1.0f);
        CHECK(s.tone.lowDb > 1.0f && s.tone.airDb > 1.0f && s.gainDb < 7.45f);
    }
    {   // A measurement too quiet to judge sets nothing.
        mf::Measurement m = onLine(-6.0f, 0.0f, 0.0f);
        m.valid = false;
        const mf::Settings s = mf::derive(m, youtube, neutral, 1.0f);
        CHECK(s.tone.tiltDb == 0.0f && s.gainDb == 0.0f && s.glueDb == 0.0f);
    }
    {   // Glue: a detector held at -10 dB; 2 dB of mean reduction at 2:1 wants 4 dB over the
        // threshold, past the 3 dB half-knee: the threshold 4 dB under the level (its bin's centre).
        mf::Measurement m = onLine(-4.0f, 0.0f, 0.0f);
        const int bin = static_cast<int>((-10.0f - mf::kHistLoDb) / mf::kHistStepDb);
        m.glueHist[bin] = 1000;
        const float level = mf::kHistLoDb + (static_cast<float>(bin) + 0.5f) * mf::kHistStepDb;
        const float thr = mf::glueThresholdFor(m, 2.0f);
        CHECK(std::fabs(thr - (level - 4.0f)) < 0.01f);
        CHECK(std::fabs(mf::glueReductionDb(m, thr) - 2.0f) < 0.01f);
        CHECK(mf::glueReductionDb(m, 0.0f) == 0.0f);
        CHECK(mf::glueThresholdFor(m, 0.0f) == 0.0f);    // none wanted: the threshold at the top
        CHECK(mf::glueThresholdFor(m, 40.0f) == -60.0f); // more than it can give: as far as it goes
    }
    {   // The glue rule: none for a dense section, 1 dB of cohesion from 12 dB of PLR on, plus 30% of
        // what stands over the target's room, at most 4 dB.
        CHECK(mf::glueAmountFor(8.0f, 11.5f) == 0.0f);
        CHECK(std::fabs(mf::glueAmountFor(10.0f, 11.5f) - 0.5f) < 1e-5f);
        CHECK(std::fabs(mf::glueAmountFor(12.0f, 11.5f) - 1.15f) < 1e-5f);
        CHECK(std::fabs(mf::glueAmountFor(18.0f, 11.5f) - 2.95f) < 1e-5f);
        CHECK(mf::glueAmountFor(30.0f, 11.5f) == 4.0f);
        CHECK(mf::glueAmountFor(20.0f, 99.0f) == 1.0f);   // Live: cohesion only
        bool inRange = true;
        for (float plr = 0.0f; plr <= 40.0f; plr += 0.5f)
            for (float room = 0.0f; room <= 20.0f; room += 2.5f) {
                const float g = mf::glueAmountFor(plr, room);
                inRange = inRange && g >= 0.0f && g <= mf::kGlueMaxDb;
            }
        CHECK(inRange);
        // derive() gives the target's room: the louder the target, the more glue.
        mf::Measurement m = onLine(sc.slope, sc.subDb, sc.airDb, -20.0f);
        m.truePeakDb = -2.0f;   // PLR 18
        const float yt = mf::derive(m, youtube, neutral, 1.0f).glueDb, ld = mf::derive(m, loud, neutral, 1.0f).glueDb;
        CHECK(std::fabs(yt - 2.95f) < 1e-4f);
        CHECK(ld > yt && ld <= 4.0f);
        CHECK(mf::derive(m, live, neutral, 1.0f).glueDb == 1.0f);
    }
    {   // A section too quiet for the gain's range: the gain at its top, the shortfall reported.
        const mf::Settings s = mf::derive(onLine(sc.slope, sc.subDb, sc.airDb, -38.0f), youtube, neutral, 1.0f);
        CHECK(s.gainDb == mf::kMaxGainDb);
        CHECK(std::fabs(s.shortDb - (38.0f - 12.5f - mf::kMaxGainDb)) < 0.05f);
        CHECK(mf::derive(onLine(sc.slope, sc.subDb, sc.airDb, -20.0f), youtube, neutral, 1.0f).shortDb == 0.0f);
    }
}
