# MasterForce

**The last insert on the Akai Force's master: the end of the chain while you play, and a master good
enough for a YouTube upload after one press.**

A sibling of [EffectForce](https://github.com/Devko/EffectForce), [PolyForce](https://github.com/Devko/PolyForce)
and [SubForce](https://github.com/Devko/SubForce), on the same groundwork. Without setup it never lets the
master clip and changes nothing below its ceiling. Press LEARN, play the loudest 8 bars, and it sets a
broad tone, a little glue and the gain for a loudness target (YouTube −14 LUFS, Loud −10), under a
true-peak limiter at −1 dBTP. It is a safe finisher, not a mastering engineer: [Design](docs/DESIGN.md).

> [!NOTE]
> **Early (prototype).** The chain and LEARN exist as DSP with tests and an offline prototype that
> masters a WAV. The plugin itself (pages, parameters, package) comes next.

## The chain

```
IN → Tone → Glue → Gain → Warmth → Width / mono bass → Clipper (2×) → Limiter (true peak) → OUT
```

| Stage | |
|---|---|
| Tone | Tilt, Low (80 Hz), Air (10 kHz): broad shelves only |
| Glue | A gentle 2:1 compressor; its threshold comes from the measurement, its makeup keeps the level |
| Gain | To the target, set by LEARN |
| Warmth | Tube saturation, level-compensated: even harmonics and a little density (off by default) |
| Width / mono bass | The side's level; the low end in the middle (80, 120 or 160 Hz) |
| Clipper | Shaves the fastest peaks before the limiter, only while the chain drives |
| Limiter | Look-ahead (64 samples, 1.45 ms), true peak, −1 dBTP |

## The prototype

Linux or WSL, with g++ (and arm-linux-gnueabihf-g++ and qemu-arm for `make test-arm`):

```sh
make test                   # the suite under ASan/UBSan
make test-arm               # the same, built for the Force, under qemu-arm
make proto                  # build/mfproto
build/mfproto master in.wav out.wav --bpm 90                     # LEARN on the loudest 8 bars, YouTube
build/mfproto master in.wav out.wav --bpm 90 --learn-bar 33 --target loud --style warm --warmth 40
build/mfproto analyze reference.wav                              # a track's bands and STYLE numbers
```

44.1 kHz WAV in (PCM 16/24/32 or float), 24-bit WAV out. `tools/mfproto.cpp` lists every option.

## License

MIT, see [LICENSE](LICENSE). `dsp/` holds EffectForce's shared pieces (simd, fastmath, the SVF, the
halfbands, EQ, Comp, Drive) as they were at its 0.0.2.
