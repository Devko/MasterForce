#include "master_map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mf {

// The option lists in surface.py against the DSP's.
static_assert(kTargetOptions == kNumTargets && kStyleOptions == kNumStyles, "surface.py TARGETS / STYLES");
static_assert(PARAM_INFO[P_TARGET].nopts == kNumTargets && PARAM_INFO[P_STYLE].nopts == kNumStyles,
              "surface.py TARGETS / STYLES must match dsp/learn.h kTargets / kStyles");
static_assert(sizeof kMonoHz / sizeof kMonoHz[0] == static_cast<size_t>(PARAM_INFO[P_MONO].nopts), "surface.py MONO_HZ");

float paramValue(int id, float n) {
    if (id < 0 || id >= P_COUNT) return 0.0f;
    const ParamSpec& s = PARAM_SPECS[id];
    n = n > 0.0f ? (n < 1.0f ? n : 1.0f) : 0.0f;   // NaN-safe (std::clamp passes NaN through)
    switch (s.curve) {
        case Curve::Lin:  return s.lo + n * (s.hi - s.lo);
        case Curve::Log:  return s.lo * std::pow(s.hi / s.lo, n);
        case Curve::Int:  return std::round(s.lo + n * (s.hi - s.lo));
        case Curve::Enum: return std::round(n * s.hi);   // lo = 0, hi = options - 1
        case Curve::Pow:  return s.hi * n * n * n;
        default:          return 0.0f;
    }
}

float paramNorm(int id, float v) {
    if (id < 0 || id >= P_COUNT) return 0.0f;
    const ParamSpec& s = PARAM_SPECS[id];
    float n = 0.0f;
    switch (s.curve) {
        case Curve::Log: n = v > 0.0f ? std::log(v / s.lo) / std::log(s.hi / s.lo) : 0.0f; break;
        case Curve::Pow: n = s.hi > 0.0f && v > 0.0f ? std::cbrt(v / s.hi) : 0.0f; break;
        case Curve::Enum: n = s.hi > 0.0f ? std::round(v) / s.hi : 0.0f; break;
        case Curve::Lin:
        case Curve::Int: n = s.hi > s.lo ? (v - s.lo) / (s.hi - s.lo) : 0.0f; break;
        default: break;
    }
    // A value too big for float's arithmetic (a hand-edited 1e38) is the top of the range, not the
    // bottom; only a NaN falls back to 0.
    return std::isnan(n) ? 0.0f : std::clamp(n, 0.0f, 1.0f);
}

std::string paramDisplay(int id, float norm) {
    if (id < 0 || id >= P_COUNT) return {};
    const float v = paramValue(id, norm);
    char b[48];
    switch (PARAM_SPECS[id].fmt) {
        case Fmt::Enum:
        case Fmt::Text: {
            const int i = static_cast<int>(v);
            return i >= 0 && i < PARAM_INFO[id].nopts ? PARAM_INFO[id].opts[i] : "";
        }
        case Fmt::Percent: std::snprintf(b, sizeof b, "%.0f%%", v * 100.0f); break;
        case Fmt::Db: std::snprintf(b, sizeof b, "%+.1f dB", std::fabs(v) < 0.05f ? 0.0f : v); break;   // never "-0.0 dB"
        case Fmt::DbTp: std::snprintf(b, sizeof b, "%.1f dBTP", std::fabs(v) < 0.05f ? 0.0f : v); break;
        default: return {};
    }
    return b;
}

// A value within a hair of a stage's rest is its rest exactly: 0 dB is 0.4 of Gain's -12..+18, which
// float makes 0.0000005 dB, and that would wake the gain stage and the clipper (and Width at 1 its M/S),
// so the chain at its defaults would no longer pass its input bit for bit.
float snap(float v, float rest, float hair) { return std::fabs(v - rest) < hair ? rest : v; }

Master::Params masterParams(const float* n, float glueThresholdDb) {
    auto v = [n](int id) { return snap(paramValue(id, n[id]), 0.0f, 1e-3f); };
    Master::Params p;
    p.tone.tiltDb = v(P_TILT);
    p.tone.lowDb = v(P_LOW);
    p.tone.airDb = v(P_AIR);
    p.glueDb = v(P_GLUE);
    p.glueThresholdDb = glueThresholdDb;
    p.gainDb = snap(std::clamp(v(P_GAIN) + v(P_LOUDNESS), kMinGainDb, kMaxGainDb), 0.0f, 1e-3f);   // LOUDNESS trims what LEARN set
    p.warmth = v(P_WARMTH);
    p.width.width = snap(v(P_WIDTH), 1.0f, 1e-4f);
    p.width.monoHz = kMonoHz[std::clamp(static_cast<int>(v(P_MONO)), 0, PARAM_INFO[P_MONO].nopts - 1)];
    p.ceilingDb = v(P_CEILING);
    return p;
}

} // namespace mf
