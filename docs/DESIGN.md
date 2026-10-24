# MasterForce: design

A sibling of EffectForce for the last insert on the Force's master. While
you play it is the end of the chain: never too loud, never clipping. After one press it gives a master
good enough for a YouTube upload. It is a safe finisher, not LANDR and not a mastering engineer.

Decided with the user (2026-10-08 to 2026-10-24):

- **Quality bar:** good enough for a YouTube or similar upload. Not a professional master, not LANDR.
- **Two jobs in one:** end of chain (always on, safe with no setup) and mastering (LEARN once).
- **LEARN on a clear input, not the whole song:** the user plays the loudest section. Whole-song
  analysis, reference matching, genre detection and trained models are out.
- **A plugin of its own**, not an EffectForce module: EffectForce's budget is spent (everything on,
  p99 14.9 of 15), its parameter list freezes at v0.1, and a limiter has to be the last insert. On the
  master: EffectForce first, this one after it. Named MasterForce, in a repo of its own (2026-10-24).
- **The rest was handed over** ("design it for me, decide for me", 2026-10-24): the glue rule, the
  STYLE curves and Warmth below are Claude's calls, each with its reasons where it's described.

## What the Force does

From the user (2026-10-24) and EffectForce's [probe](https://github.com/Devko/EffectForce/blob/main/docs/PROBE.md):

| | |
|---|---|
| Inserts on the master | **Yes**, on Master Out 1/2 |
| Master fader | After the inserts, by the user's reading (not measured). Either way the rule is the same: export with the master fader at 0 dB |
| Export | The save screen's audio mixdown (Arranger only) writes the audio out to a WAV, **master inserts included**. Whether it renders in real time isn't known, so nothing in the audio path may depend on the clock |
| From the probe | 44.1 kHz; always 128-frame blocks, in place; tempo and song position valid; no latency compensation (`acceptIOChanges` no); no file selector; Stop suspends and resumes within 100 ms; the getters polled hundreds of times a second |

## Behaviour

A fixed chain:

```
IN → Tone → Glue → Gain → Warmth → Width / mono bass → Clipper (2×) → Limiter (true peak, 64-sample look-ahead) → OUT
```

- **Without LEARN** it is the end of the chain: Tone flat, Glue off, Gain 0 dB, Warmth 0, the clipper
  off, the limiter at −1 dBTP. Below the ceiling it changes nothing: the output is the input 64 samples late,
  bit for bit (test/master_test.cpp).
- **LEARN:** press it and the next 8 bars from the bar line are measured (the looper's grid). Play the
  loudest section (the drop, the chorus). While it measures, the line shows `LEARN 3/8`. At the end it
  sets the knobs and says what it did (`LEARNED: tilt +1.2 · glue 2.1 dB · +4.5 dB`). It refuses a
  section too quiet to judge (gated loudness under −40 LUFS), a transport stopped halfway or a jump in
  the song position (`LEARN: play 8 bars`). A second press while it measures cancels. A section so
  quiet that the gain's +18 dB can't reach the target should say so too (`LEARNED: 3 dB short`): the
  prototype ran into that case (a beat at −33 LUFS), and the page doesn't show it yet.
- **What LEARN sets:** Tone (Tilt, Low, Air), Glue's threshold and makeup, and the input Gain. These
  are ordinary parameters, saved and automatable like any other, and nothing is hidden. Width, mono
  bass and the ceiling stay the user's.
- **The measurement is kept** (one state line), so changing TARGET, STYLE or AMOUNT applies again at
  once without playing again. A learned knob turned by hand keeps its value until then.
- **TARGET:** YouTube (−14 LUFS) · Loud (−10) · Live (no drive: LEARN sets Tone and Glue only, and the
  limiter is a safety). The learned section is aimed **1.5 dB above** the target, because a song's
  quieter parts bring its integrated loudness down by about that much. YouTube turns louder uploads
  down to about −14 and doesn't raise quieter ones.
- **STYLE:** Warm · Neutral · Bright: the tone LEARN moves toward (a slope, plus sub and air offsets).
  It doesn't touch WARMTH: STYLE is where LEARN aims the tone, WARMTH a sound of its own, and Warm
  with no saturation (or Bright with some) are both fair choices.
- **AMOUNT** 0–100%: how far Tone and Glue go toward it. **LOUDNESS** ±6 dB: a trim on the gain.
- **WARMTH** 0–100%: saturation (below). Off by default.
- **One page.** Q-Links: AMOUNT, LOUDNESS, TILT, LOW, AIR, GLUE, WIDTH, CEILING. Buttons: LEARN, TARGET,
  STYLE, MONO BASS (Off · 80 · 120 · 160 Hz).
- **Meters are text**, updated as EffectForce's CPU readout and scenes' line are
  (`surface.textsChanged()`): `-13.9 LUFS · song -14.2 · TP -1.0 · GR 2.3`. That is short-term (3 s),
  the song (integrated and gated, since play started), the highest true peak since play started, and
  the gain reduction now (clipper and limiter). Below it: `Meters read before the master fader: export
  at 0 dB`.

## DSP

| Stage | What | From |
|---|---|---|
| Tone | Tilt (a low and a high shelf in opposite directions, pivoting about 1 kHz), Low (shelf, 80 Hz), Air (shelf, 10 kHz). Knobs ±6 dB; LEARN moves each at most ±3 | dsp/eq's shelves |
| Glue | Feed-forward, stereo-linked, 2:1, 6 dB soft knee, 30 ms attack, 300 ms release, sidechain low cut 100 Hz (the kick doesn't pump it) | dsp/comp, Comp mode as it is |
| Width / mono bass | M/S. Side 0–150%. Mono bass: a 4th-order high-pass on the side | new, a few lines; the OTT's LR4 sections |
| Clipper | Soft (straight up to 1.9 dB under its level, then a parabola up to it), 2× oversampled, its level 1 dB over the ceiling. It works only while Gain drives (above 0 dB) | new; dsp/halfband.h |
| Limiter | Stereo-linked. 64 samples of look-ahead: the true-peak detector's 12, a 45-sample (1 ms) fall, and the lowest gain held 8 samples either side of a peak, because a point between two samples is rebuilt from the samples round it. Release 40–300 ms, by how dense the overs are. It is kept as the reduction: a gain creeping back to 1 stalls a few ulps short in float and would never be bit for bit again. A hard clip at the ceiling after it, for safety | new |
| True peak | 4× oversampled, a 24-tap-per-phase Kaiser sinc: flat within 0.01 dB to 18 kHz. BS.1770's own 12-tap filter falls 0.2 dB by 15 kHz and 2 dB by 18 kHz, and a limiter that trusted it let music-like noise through 0.3 dB over. What 4× can't catch is energy at 20–22 kHz (white noise reads 0.5 dB low); music has little there, and YouTube's encoders remove it | new |
| Meters | BS.1770: K-weighting, 400 ms and 3 s blocks, gating for the song value (a 0.1 dB histogram). Their own true-peak detector, on the output | tools/loudness.h's K-weighting, made real-time |

- **Latency:** the look-ahead delays the output by 64 samples, reported as 0. On the master nothing
  runs beside it, so nothing can be out of line with it. On a track it would be, so the user guide says
  master only.
- **LEARN's measurement** runs on the audio thread, and only while learning. It collects octave bands
  from 31.25 Hz to 16 kHz (10 band-passes on the mid), a histogram of the glue detector's level (0.5 dB
  bins; the detector rebuilt exactly as Comp's, so the mean reduction of any threshold follows from it),
  the gated loudness and the true peak. The fit at the end is a few dozen operations. The results
  reach the knobs as a preset load's do (`hostAutomate` from the UI thread), never from the audio
  thread.
- **CPU:** p99 ≤ 4% of the block, measured on the device as EffectForce's (`make bench-device`).

### What LEARN computes

The prototype decides the numbers.

- **Tilt:** the least-squares slope (dB per octave) of the bands from 125 Hz to 8 kHz, against
  STYLE's slope. The difference × AMOUNT, at most ±3 dB. One dB of Tilt moves the slope by about
  0.17 dB an octave, so LEARN corrects at most about half a dB an octave: broad strokes only.
- **Low, Air:** how far the 31.5–63 Hz and 16 kHz bands sit from the fitted line, against STYLE's
  offsets. × AMOUNT, at most ±3 dB.
- **Glue:** the threshold that gives the wanted reduction over the histogram, and makeup to match it.
  The wanted reduction (`glueAmountFor`) comes from the section's peak-to-loudness ratio (PLR) and the
  room the target leaves its peaks (the ceiling over the section's aim: 11.5 dB for YouTube, 7.5 for
  Loud):
  - **cohesion:** none at a PLR of 8 dB (a dense loop, already squashed: glue would only pump), rising
    to 1 dB at 12 and staying there
  - **plus 30% of the excess** (PLR over the room): glue takes the slow part of the work, the clipper
    and the limiter the fast part, so none of them is pushed into pumping or crunching
  - **at most 4 dB**: past that a 2:1 compressor is audibly working, not gluing
  So a dynamic section (PLR 18) gets 2.95 dB for YouTube and 4 for Loud, a dense one (PLR 10) half
  a dB, and Live (no drive) only the cohesion.
- **STYLE curves:** octave bands read pink noise flat. A balanced commercial mix falls about 4.5 dB an
  octave per hertz (which is why spectrum analyzers tilt by 4.5, Voxengo SPAN's and FabFilter's
  default, to show one flat), 1.5 steeper than pink: **Neutral −1.5 dB/octave**, the sub 1 dB under
  the line (the 63 Hz octave on it, the 31 Hz one below), the 16 kHz octave 4 dB under (mixes fall away
  up there, and lossy encoders cut it). **Warm** −2.0, sub on the line, air −5.5; **Bright** −1.0, sub
  −2, air −2.5. Starting values from that convention, not from measuring tracks: `mfproto analyze`
  on references refines them. (The prototype's first placeholders said −4.5: per hertz, not per
  octave band, so 3 dB an octave too dark, and every move hit its limit.)
- **Gain:** target + 1.5 − the section's loudness after Tone and Glue (estimated from the bands and the
  reduction). On the Loud target it may come out about a dB low, because the limiter's work takes some
  loudness off. The song meter and LOUDNESS cover the difference. The gain goes from −12 to +18 dB; a
  section quieter than that reaches says by how much (`Settings::shortDb`).

### Warmth

Saturation the way an analog box on the master gives it: even harmonics, a little density, a softer
top. One knob, **WARMTH** 0–100%, off at 0 (bit for bit). It is EffectForce's Drive, Tube: a triode's
curve, even harmonics at every level (no bias needed), its gain compensation keeping the level, and a
mix against a phase-aligned dry (no combing). WARMTH is the mix, the drive (0 to 6 dB) and a touch of
darkness (Drive's tone to −0.1) together: up to half way a little density and a soft second harmonic
(about 1.6%), at full plainly saturated. After Gain, so it always meets the mastered level and a
setting sounds the same whatever comes in; before the clipper, which then has less to do. About 1.4%
of the block. LEARN doesn't set it: it's taste.

Waking it took two fixes, both checked by test/master_test.cpp: its mix moves at most 1/16 a chunk
(12 ms end to end), and while it's off the chain keeps the last 256 samples it would have had and
plays them through it first when it wakes. Without that, Drive's halfbands and its dry path start
from silence and ring for a few hundred samples as the music arrives.

## Not in it

Whole-song analysis, reference matching, genre detection and trained models (the quality bar doesn't
need them). Multiband dynamics: a 3-band compressor set automatically is the easiest way to make a mix
worse, and one band of glue is enough. A de-esser. Dither (YouTube re-encodes anyway). A spectrum
display (MPC skins can't draw one).

## Plan

1. ✅ **Prototype offline** (2026-10-24): `make proto`, then `build/mfproto master` reads a WAV, runs
   LEARN on 8 bars (`--learn-bar`, or the loudest 8), writes the master and prints what LEARN set and
   the meters. `build/mfproto analyze` prints a track's bands and its STYLE numbers. The suite
   (`make test`, `make test-arm`) checks every stage against what it promises: 115 checks. On a
   synthetic beat: YouTube −14.3 LUFS (target −14), Loud −10.4 (target −10), true peak −1.00 dBTP.
   What to listen for on real beats: the synthetic one came out darker than its target (−2.2 dB an
   octave, aimed at −1.5) although Tilt brightened it, because its drums are very spiky (PLR 18.8) and
   the clipper and the limiter took their bright transients down hard. Real mixes are less spiky.
2. **By ear:** a few of the user's beats, mastered by the prototype and by AIR Maximizer set by hand,
   uploaded unlisted to YouTube and listened to on a phone and a laptop. STYLE's three curves get tuned
   here.
3. **The plugin:** the siblings' groundwork brought here (surface.py, the plugin glue, release.py, the
   catalog check). One page, then the bench on the device.

## Open

- STYLE's curves measured on references (step 2), against the starting values above.
- Mono bass's default: Off (the end of the chain changes nothing below the ceiling) or 120 Hz.
- Whether the master fader really comes after the inserts (EffectForce's Drive turned up, the master
  fader moved), for the user guide's wording.
