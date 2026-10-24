// The meter against BS.1770's own calibration and its gating.
#include "measure.h"
#include "../dsp/loudness.h"

using namespace eft;

void loudnessTests() {
    {   // 997 Hz at -20 dBFS in both channels reads -20 LUFS (BS.1770: 0 dBFS in one channel, -3.01).
        mf::Loudness m;
        const Buf x = fadeIn(sine(997.0, static_cast<int>(5 * ef::kRate), 0.1f));
        m.process(x.data(), x.data(), static_cast<int>(x.size()));
        CHECK(std::fabs(m.integrated() + 20.0f) < 0.05f);
        CHECK(std::fabs(m.momentary() + 20.0f) < 0.05f);
        CHECK(std::fabs(m.shortTerm() + 20.0f) < 0.05f);
        CHECK(std::fabs(m.truePeakDb() + 20.0f) < 0.05f);
    }
    {   // One channel only: 3 dB less.
        mf::Loudness m;
        const Buf x = sine(997.0, static_cast<int>(5 * ef::kRate), 0.1f), z(x.size(), 0.0f);
        m.process(x.data(), z.data(), static_cast<int>(x.size()));
        CHECK(std::fabs(m.integrated() + 23.01f) < 0.05f);
    }
    {   // Gating: 5 s at -20 then 5 s at -50 reads -20 (the quiet half is under the relative gate);
        // silence reads nothing.
        mf::Loudness m;
        const int n = static_cast<int>(5 * ef::kRate);
        Buf x = sine(997.0, 2 * n, 0.1f);
        for (int i = n; i < 2 * n; ++i) x[static_cast<size_t>(i)] *= 0.0316228f;
        m.process(x.data(), x.data(), 2 * n);
        CHECK(std::fabs(m.integrated() + 20.0f) < 0.2f);
        CHECK(std::fabs(m.shortTerm() + 50.0f) < 0.1f);

        mf::Loudness quiet;
        const Buf z(static_cast<size_t>(n), 0.0f);
        quiet.process(z.data(), z.data(), n);
        CHECK(quiet.integrated() == mf::Loudness::kSilence);
        CHECK(quiet.truePeakDb() == mf::Loudness::kSilence);
    }
    {   // The true peak between the samples: a quarter-rate sine whose samples read 3 dB low.
        mf::Loudness m;
        const Buf x = fadeIn(sine(ef::kRate / 4.0, 44100, 0.5f, kPi / 4.0));
        m.process(x.data(), x.data(), static_cast<int>(x.size()));
        CHECK(std::fabs(m.truePeakDb() + 6.02f) < 0.05f);
    }
    {   // The weighting LEARN estimates with: +0.69 dB at 997 Hz (what BS.1770's -0.691 takes back),
        // +4 dB high up, the low end cut.
        CHECK(std::fabs(mf::Loudness::weightingDb(997.0f) - 0.691f) < 0.05f);
        CHECK(std::fabs(mf::Loudness::weightingDb(10000.0f) - 4.0f) < 0.3f);
        CHECK(mf::Loudness::weightingDb(31.25f) < -3.0f);
    }
}
