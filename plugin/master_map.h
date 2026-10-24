#pragma once
// MPC's 0..1 parameter values <-> real values, display text, and the chain's parameters.
// Ranges, curves, names and options come from surface/surface.py via build/param_ids.h.
#include "param_ids.h"
#include "../dsp/master.h"

#include <string>

namespace mf {

float paramValue(int id, float norm);    // real value (dB, 0..1, option index...)
float paramNorm(int id, float value);    // inverse, for state text, tests and the bench
std::string paramDisplay(int id, float norm);   // what the knob's value label shows

// The chain's parameters from every parameter's 0..1 value (norm[P_COUNT]); the glue's threshold
// comes from the measurement (Surface::glueThresholdDb), not from a parameter.
Master::Params masterParams(const float* norm, float glueThresholdDb);

} // namespace mf
