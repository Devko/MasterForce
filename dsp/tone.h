#pragma once
// Tone: Tilt, Low and Air, broad moves only. Two EffectForce EQs (dsp/eq.h) with nothing but their
// shelves in use:
//
//   Tilt   a low shelf at 250 Hz by -tilt/2 and a high shelf at 4 kHz by +tilt/2: the bottom down
//          and the top up (or the other way), about 0 dB round 1 kHz
//   Low    a low shelf at 80 Hz
//   Air    a high shelf at 10 kHz
//
// Flat, each EQ passes its input through bit for bit and costs nothing (eq.h).
//
// responseDb() is the analog response the EQ's prewarped shelves match (exactly at the corners,
// within hundredths below 10 kHz): LEARN estimates with it what its moves do to the bands and the
// loudness.
#include "eq.h"

#include <cmath>
#include <cstring>

namespace mf {

class Tone {
public:
    struct Params {
        float tiltDb = 0.0f;   // -6..+6
        float lowDb = 0.0f;    // -6..+6
        float airDb = 0.0f;    // -6..+6
    };
    static constexpr float kTiltLowHz = 250.0f, kTiltHighHz = 4000.0f, kLowHz = 80.0f, kAirHz = 10000.0f;
    static constexpr float kRange = 6.0f;

    void reset() {
        tilt_.reset();
        ends_.reset();
        fresh_ = true;
    }
    void set(const Params& p, const ef::Transport& t) {
        // The same knobs as the last chunk: the EQs would only find that out again (nine clamps and
        // compares each), their glides go on either way.
        if (!fresh_ && std::memcmp(&p, &last_, sizeof p) == 0) return;
        last_ = p;
        fresh_ = false;
        ef::Eq::Params a, b;
        const float tilt = ef::clampParam(p.tiltDb, -kRange, kRange, 0.0f);
        a.lowFreq = kTiltLowHz;
        a.lowGainDb = -0.5f * tilt;
        a.highFreq = kTiltHighHz;
        a.highGainDb = 0.5f * tilt;
        b.lowFreq = kLowHz;
        b.lowGainDb = ef::clampParam(p.lowDb, -kRange, kRange, 0.0f);
        b.highFreq = kAirHz;
        b.highGainDb = ef::clampParam(p.airDb, -kRange, kRange, 0.0f);
        tilt_.set(a, t);
        ends_.set(b, t);
    }
    void process(float* L, float* R, int n) {
        tilt_.process(L, R, n);
        ends_.process(L, R, n);
    }

    static float responseDb(const Params& p, float hz) {
        return shelfDb(hz, kTiltLowHz, -0.5f * p.tiltDb, true) + shelfDb(hz, kTiltHighHz, 0.5f * p.tiltDb, false) +
               shelfDb(hz, kLowHz, p.lowDb, true) + shelfDb(hz, kAirHz, p.airDb, false);
    }

private:
    // RBJ's analog shelf at Q 0.707 (no overshoot), half its gain at the corner; w through the
    // bilinear transform's warp, as the EQ is prewarped.
    static float shelfDb(float hz, float corner, float gainDb, bool low) {
        if (gainDb == 0.0f) return 0.0f;
        const double pi = 3.14159265358979323846, sr = ef::kRate;
        const double a = std::pow(10.0, gainDb / 40.0), s = std::sqrt(a) * 1.41421356237;   // sqrt(A) / Q
        const double w = std::tan(pi * std::min<double>(hz, 0.49 * sr) / sr) / std::tan(pi * corner / sr), w2 = w * w;
        double nr, ni, dr, di;
        if (low) {
            nr = a * (a - w2), ni = a * s * w;
            dr = 1.0 - a * w2, di = s * w;
        } else {
            nr = a * (1.0 - a * w2), ni = a * s * w;
            dr = a - w2, di = s * w;
        }
        return static_cast<float>(10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di)));
    }

    ef::Eq tilt_, ends_;
    Params last_;
    bool fresh_ = true;
};

} // namespace mf
