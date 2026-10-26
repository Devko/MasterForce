# Changelog

## 0.0.1

The first build: MasterForce as a VST2 insert for MPC's master on the Force.

- The end of the chain: a true-peak look-ahead limiter at −1 dBTP (64 samples, 1.45 ms); below its
  ceiling the master passes untouched, bit for bit.
- LEARN: play the loudest 8 bars and it sets Tilt, Low, Air, Glue and Gain for a target (YouTube −14
  LUFS, Loud −10, Live) and a style (Warm, Neutral, Bright); TARGET, STYLE and AMOUNT re-derive them
  without playing again, and the measurement is saved with the project. It counts its 8 bars from the
  tempo and the time played, so a looping sequence or clip, a locate, or MPC's song position stepping
  unevenly doesn't stop it (the first device run said "the song jumped"); only stopping does.
- Warmth (tube saturation, level kept), width, mono bass, a 2x clipper before the limiter.
- The meters on the top line: short-term and song loudness, true peak, gain reduction, CPU.
- 15 factory presets in four categories; user presets, favorites, the browser.
- Lighter on the Force's CPU: the true-peak detectors, the limiter, the meters and LEARN's measuring
  run in vector lanes, without scalar float compares, which stall ARMv7. That is 55–60% fewer ARM
  instructions at rest and while LEARN measures, and about 45% fewer with everything on. The sound is
  the same.
- Browsing presets after a LEARN no longer drops out for a moment: a preset and the knobs LEARN
  derives from it reach the audio in one piece, GLUE's threshold with them.
- Two LEARN presses while the slot is off cancel each other, and a corrupt measurement in a project
  is ignored instead of turning the learned knobs to NaN.
