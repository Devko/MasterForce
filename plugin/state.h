#pragma once
// The plugin's state text, shared by projects (effGetChunk / effSetChunk) and preset files.
//
// "masterforce 1": key=value lines for every sound parameter: REAL values (dB, 0..1) and options by
// name ("target=Loud", "mono=120 Hz"; an index is read too). A project adds the preset it came from
// and LEARN's measurement (one learn= line, below): the song's, so a project reopens with it and
// TARGET, STYLE and AMOUNT keep working. A preset says how to master, never what LEARN measured or
// set: it has no learn= line and no Tilt, Low, Air, Glue or Gain (surface.py LEARNED_KEYS).
// Survives parameters being added or reordered AND ranges changing.
//
// learn=1;<loudness>;<true peak>;<10 bands, comma-separated>;<the glue histogram's counts, a run of
// zeros as 0*N>
#include "surface.h"

#include <string>

namespace mf {

constexpr int kStateVersion = 1;

std::string saveState(const Surface& s, bool asPreset);
bool isStateText(const std::string& text);   // "masterforce <version >= 1>" (a UTF-8 BOM allowed)
// A preset starts from the defaults (what it doesn't say is the default) and leaves the measurement
// as it is; a project's state only overrides what it lists, and brings its own measurement (none
// listed: none). False if it isn't MasterForce state.
bool loadState(Surface& s, const std::string& text, bool asPreset);

std::string measurementText(const Measurement& m);
bool parseMeasurement(const std::string& text, Measurement& m);   // false (m invalid) if it isn't one

} // namespace mf
