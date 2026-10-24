# Changelog

## 0.0.1

The first build: MasterForce as a VST2 insert for MPC's master on the Force.

- The end of the chain: a true-peak look-ahead limiter at −1 dBTP (64 samples, 1.45 ms); below its
  ceiling the master passes untouched, bit for bit.
- LEARN: play the loudest 8 bars and it sets Tilt, Low, Air, Glue and Gain for a target (YouTube −14
  LUFS, Loud −10, Live) and a style (Warm, Neutral, Bright); TARGET, STYLE and AMOUNT re-derive them
  without playing again, and the measurement is saved with the project.
- Warmth (tube saturation, level kept), width, mono bass, a 2x clipper before the limiter.
- The meters on the top line: short-term and song loudness, true peak, gain reduction, CPU.
- 15 factory presets in four categories; user presets, favorites, the browser.
