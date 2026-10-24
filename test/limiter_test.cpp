// The limiter: under the ceiling a delay, bit for bit; over it, the true peak held under the ceiling.
#include "measure.h"
#include "../dsp/limiter.h"

using namespace eft;

namespace {

void run(mf::Limiter& lim, Buf& L, Buf& R) {
    for (size_t i = 0; i < L.size(); i += ef::kChunk) {
        const int n = static_cast<int>(std::min<size_t>(ef::kChunk, L.size() - i));
        lim.set(mf::Limiter::Params{});
        lim.process(L.data() + i, R.data() + i, n);
    }
}

bool delayedCopy(const Buf& in, const Buf& out, int latency, size_t from = 0) {
    for (size_t i = std::max<size_t>(from, static_cast<size_t>(latency)); i < out.size(); ++i)
        if (out[i] != in[i - static_cast<size_t>(latency)]) return false;
    return true;
}

} // namespace

void limiterTests() {
    const int n = static_cast<int>(ef::kRate);   // 1 s
    const double ceil = std::pow(10.0, -1.0 / 20.0);
    CHECK(mf::Limiter::kLatency == 64);

    {   // Below the ceiling: the input, kLatency samples late, bit for bit; silence before it.
        mf::Limiter lim;
        const Buf inL = whiteNoise(n, 0.3f, 1), inR = whiteNoise(n, 0.3f, 2);
        Buf L = inL, R = inR;
        run(lim, L, R);
        CHECK(delayedCopy(inL, L, mf::Limiter::kLatency));
        CHECK(delayedCopy(inR, R, mf::Limiter::kLatency));
        CHECK(peak(L, 0, mf::Limiter::kLatency) == 0.0f);
        CHECK(lim.takeReductionDb() == 0.0f);
    }
    {   // Hot, program-like (its energy low): no sample past the ceiling, the true peak within 0.1 dB.
        mf::Limiter lim;
        Buf L = softNoise(n, 2.0f, 3), R = softNoise(n, 2.0f, 4);
        run(lim, L, R);
        CHECK(peak(L) <= static_cast<float>(ceil) && peak(R) <= static_cast<float>(ceil));
        const double tp = truePeakRef(L, R);
        CHECK(dbOf(tp) <= -1.0 + 0.1);
        CHECK(lim.takeReductionDb() > 3.0f);
    }
    {   // Hot white noise, as much energy at 20-22 kHz as anywhere: the case 4x detection can't get
        // exact (it reads white noise's peaks about 0.5 dB low). Within 1.2 dB.
        mf::Limiter lim;
        Buf L = whiteNoise(n, 2.0f, 5), R = whiteNoise(n, 2.0f, 6);
        run(lim, L, R);
        CHECK(peak(L) <= static_cast<float>(ceil) && peak(R) <= static_cast<float>(ceil));
        CHECK(dbOf(truePeakRef(L, R)) <= -1.0 + 1.2);
    }
    {   // A sine at a quarter of the rate, its peaks halfway between samples: the samples read 3 dB
        // under the true peak, which the limiter still holds to the ceiling.
        mf::Limiter lim;
        Buf L = fadeIn(sine(ef::kRate / 4.0, n, 1.0f, kPi / 4.0)), R = L;
        CHECK(std::fabs(peak(L, n / 2, n) - 0.7071f) < 1e-3f);
        run(lim, L, R);
        CHECK(dbOf(truePeakRef(L, R)) <= -1.0 + 0.05);
        CHECK(dbOf(truePeakRef(L, R)) > -1.5);   // and not much further down than it needs
    }
    {   // After a burst, it lets go: once the release has come back, a delay again, bit for bit.
        mf::Limiter lim;
        Buf inL = whiteNoise(3 * n, 0.3f, 7), inR = whiteNoise(3 * n, 0.3f, 8);
        for (int i = 0; i < n / 10; ++i) {
            inL[static_cast<size_t>(i)] *= 6.0f;
            inR[static_cast<size_t>(i)] *= 6.0f;
        }
        Buf L = inL, R = inR;
        run(lim, L, R);
        CHECK(!delayedCopy(inL, L, mf::Limiter::kLatency, 0));
        CHECK(delayedCopy(inL, L, mf::Limiter::kLatency, static_cast<size_t>(2 * n)));
        CHECK(delayedCopy(inR, R, mf::Limiter::kLatency, static_cast<size_t>(2 * n)));
    }
    {   // A peak's gain comes down smoothly: no step in the gain bigger than a window's share.
        mf::Limiter lim;
        Buf L(static_cast<size_t>(n), 0.5f), R = L;
        L[static_cast<size_t>(n / 2)] = R[static_cast<size_t>(n / 2)] = 4.0f;
        run(lim, L, R);
        float worst = 0.0f;
        for (int i = n / 2 - 70; i < n / 2 + 70; ++i)
            if (i != n / 2 + mf::Limiter::kLatency && i - 1 != n / 2 + mf::Limiter::kLatency)
                worst = std::max(worst, std::fabs(L[static_cast<size_t>(i)] - L[static_cast<size_t>(i - 1)]));
        CHECK(worst < 0.5f / mf::Limiter::kWindow + 1e-4f);
        CHECK(L[static_cast<size_t>(n / 2 + mf::Limiter::kLatency)] <= static_cast<float>(ceil));
    }
    {   // NaN and infinity in, finite out.
        mf::Limiter lim;
        Buf L = whiteNoise(4096, 0.5f, 9), R = L;
        L[100] = std::nanf("");
        R[200] = INFINITY;
        run(lim, L, R);
        CHECK(allFinite(L) && allFinite(R));
    }
}
