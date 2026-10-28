# User guide

- [Where it goes](#where-it-goes)
- [The end of the chain](#the-end-of-the-chain)
- [LEARN: a master in one press](#learn-a-master-in-one-press)
- [The knobs](#the-knobs)
- [The meters](#the-meters)
- [Exporting for YouTube](#exporting-for-youtube)
- [Presets](#presets)

---

## Where it goes

MasterWerk is the **last insert on Master Out 1/2**: on MPC's master, after anything else there
(EffectWerk's performance mixer, say). It delays the audio by 64 samples (1.45 ms) to see the peaks
coming; on the master nothing runs beside it, so nothing is out of time. On a track it would be: put
it on the master only.

## The end of the chain

As it loads, MasterWerk is a safety: a true-peak limiter at −1 dBTP. Nothing that leaves the master
goes over it, between the samples either (where a converter or YouTube's encoder would clip), and
below it the sound is untouched: bit for bit, 64 samples late. Leave it on while you play.

## LEARN: a master in one press

1. Pick a **TARGET**: YouTube (−14 LUFS, what YouTube turns everything down to), Loud (−10) or Live
   (no drive: tone and glue only, the limiter as a safety).
2. Pick a **STYLE**: Warm, Neutral or Bright, the tone LEARN aims at.
3. Press **LEARN** and play the **loudest part** of the song (the drop, the chorus). It starts on the
   next bar line (or where your loop starts over) and listens for 8 bars of playing time; the line
   says `LEARNING: BAR 3 OF 8`. A loop shorter than 8 bars is fine: it simply hears it again.
4. It sets Tilt, Low, Air, Glue and Gain and says what it did:
   `LEARNED   TILT +1.2   LOW -0.5   AIR +0.8   GLUE 2.0   GAIN +4.5`.

Change TARGET, STYLE or AMOUNT afterwards and the knobs follow at once, from what it measured: no need
to play again. The measurement is saved with the project. Stopping halfway stops LEARN (`PRESS
LEARN AGAIN`); a part too quiet to judge says so. A press while it listens cancels it.

## The knobs

| Knob | What it does |
|---|---|
| AMOUNT | How far Tone and Glue go toward the style: 0 leaves the tone alone |
| LOUDNESS | A trim on top of the gain LEARN set: ±6 dB |
| WARMTH | Tube saturation, its level kept: up to half way a little density and warmth, at full plainly saturated. Off at 0 |
| TILT, LOW, AIR | The tone: a tilt around 1 kHz, a shelf at 80 Hz, a shelf at 10 kHz. LEARN moves each at most 3 dB |
| GLUE | A gentle 2:1 compressor, set by how much it reduces on average (dB): the level stays |
| GAIN | Into the clipper and the limiter: LEARN sets it to reach the target |
| WIDTH | The side's level: 0 is mono, 150% wider |
| MONO BASS | The low end in the middle below 80, 120 or 160 Hz |
| CEILING | The limiter's ceiling, true peak: −1 dBTP leaves room for YouTube's encoder |

The first bank of Q-Links: AMOUNT, LOUDNESS, WARMTH, TILT, LOW, AIR, GLUE, CEILING; the second:
GAIN, WIDTH, MONO BASS, TARGET, STYLE and the preset.

## The meters

The top line: `-13.9 LUFS   SONG -14.2   PEAK -1.0   GR 2.3   CPU 4%`: the loudness of the last 3 s,
the song's since MPC started playing, its highest true peak, the gain reduction of the last quarter
second (the clipper and the limiter), and what MasterWerk costs.

They read **before MPC's master fader**: export with the master fader at 0 dB.

## Exporting for YouTube

1. LEARN on the loudest 8 bars with TARGET YouTube.
2. Play the song through once: SONG should read about −14 (a little under is fine; YouTube doesn't
   turn quieter songs up, so not much under). LOUDNESS trims it.
3. Master fader at 0 dB, then the save screen's audio mixdown (the Arranger's): the WAV is what you
   heard, MasterWerk included.

## Presets

A preset says **how** to master (target, style, amount, warmth, width, mono bass, ceiling); the
measurement and the knobs LEARN sets belong to the song. So loading a preset after a LEARN keeps
the measurement and sets Tilt, Low, Air, Glue and Gain again for the preset's choices. SAVE writes a
user preset the same way. The factory set: YouTube (Init, Clean, Warm, Bright, Wide), Loud (Loud, Loud
Warm, Club, Punch), Live (Safety, Live Glue, Live Warm) and Character (Tape, Lo-Fi, Air).
