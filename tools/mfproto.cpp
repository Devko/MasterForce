// mfproto: MasterForce's chain offline, to hear and tune it before it's a plugin (docs/DESIGN.md, "Plan").
//
//   mfproto analyze <in.wav>
//       The whole file's loudness, true peak, peak-to-loudness ratio and octave bands, and the three
//       numbers a STYLE curve is made of (slope, sub, air). Run it on commercial tracks to set the
//       curves in dsp/learn.h.
//
//   mfproto master <in.wav> <out.wav> --bpm <bpm> [options]
//       LEARN on 8 bars (as on the Force: armed, it starts on a bar line), the settings derived from
//       them, then the whole file through the chain with those settings. The output is aligned (the
//       limiter's 64 samples taken off), 24-bit.
//
//       --learn-bar N     the first of the 8 bars (1 = the file's start); default: the loudest 8
//       --target T        youtube (default), loud, live
//       --style S         warm, neutral (default), bright
//       --amount P        0..100 (100)
//       --loudness DB     the LOUDNESS trim (0)
//       --width P         0..150 (100)
//       --mono HZ         0 (off), 80, 120, 160 (0)
//       --warmth P        0..100 (0)
//       --ceiling DB      dBTP (-1)
//       --no-learn        the chain at its defaults: the end of the chain only
#include "../dsp/master.h"
#include "wav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using mf::Master;

int usage() {
    std::fprintf(stderr,
                 "usage: mfproto analyze <in.wav>\n"
                 "       mfproto master <in.wav> <out.wav> --bpm <bpm> [--learn-bar N] [--target youtube|loud|live]\n"
                 "              [--style warm|neutral|bright] [--amount 0..100] [--loudness dB] [--width 0..150]\n"
                 "              [--mono 0|80|120|160] [--ceiling dB] [--no-learn]\n");
    return 2;
}

bool load(const char* path, mfw::Audio& a) {
    std::string err;
    if (!mfw::read(path, a, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    if (a.rate != 44100) {
        std::fprintf(stderr, "%s: %d Hz; MasterForce runs at 44.1 kHz, as MPC does (convert it first)\n", path, a.rate);
        return false;
    }
    return true;
}

void printBands(const float* db) {
    std::printf("  bands  ");
    for (int b = 0; b < mf::kBands; ++b) {
        const float hz = mf::kBandHz[b];
        if (hz < 1000.0f) std::printf(" %5.0f", hz);
        else std::printf(" %4.0fk", hz / 1000.0f);
    }
    std::printf("\n  dB     ");
    for (int b = 0; b < mf::kBands; ++b) std::printf(" %5.1f", db[b]);
    std::printf("\n");
}

// The whole file through a Learner, as one long section.
int analyze(const char* path) {
    mfw::Audio a;
    if (!load(path, a)) return 1;
    mf::Learner learner;
    for (size_t i = 0; i < a.L.size(); i += ef::kChunk) {
        const int n = static_cast<int>(std::min<size_t>(ef::kChunk, a.L.size() - i));
        learner.process(a.L.data() + i, a.R.data() + i, n);
    }
    const mf::Measurement m = learner.finish();
    const mf::BandFit fit = mf::fitBands(m.bandDb);
    std::printf("%s: %.1f s\n", path, static_cast<double>(a.L.size()) / a.rate);
    std::printf("  loudness %.1f LUFS, true peak %.1f dBTP, PLR %.1f dB\n", m.loudness, m.truePeakDb, m.plr());
    printBands(m.bandDb);
    std::printf("  style   slope %.2f dB/oct, sub %+.1f dB, air %+.1f dB\n", fit.slope, fit.subDev, fit.airDev);
    return 0;
}

const char* arg(int argc, char** argv, const char* name) {
    for (int i = 0; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], name)) return argv[i + 1];
    return nullptr;
}
bool flag(int argc, char** argv, const char* name) {
    for (int i = 0; i < argc; ++i)
        if (!std::strcmp(argv[i], name)) return true;
    return false;
}
int pick(const char* v, const char* const* names, int count, int fallback) {
    if (!v) return fallback;
    for (int i = 0; i < count; ++i) {
        std::string n = names[i];
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (n == v) return i;
    }
    std::fprintf(stderr, "unknown value '%s', using %s\n", v, names[fallback]);
    return fallback;
}

// The loudest 8 bars (1-based first bar), by each bar's gated loudness.
int loudestBars(const mfw::Audio& a, double samplesPerBar) {
    const int bars = static_cast<int>(static_cast<double>(a.L.size()) / samplesPerBar);
    std::vector<double> e(static_cast<size_t>(bars), 0.0);
    for (int b = 0; b < bars; ++b) {
        mf::Loudness m;
        const size_t from = static_cast<size_t>(b * samplesPerBar), to = static_cast<size_t>((b + 1) * samplesPerBar);
        m.process(a.L.data() + from, a.R.data() + from, static_cast<int>(to - from));
        const float lu = m.integrated();
        e[static_cast<size_t>(b)] = lu > mf::Loudness::kSilence ? std::pow(10.0, lu / 10.0) : 0.0;
    }
    int best = 0;
    double bestSum = -1.0;
    for (int b = 0; b + mf::kLearnBars <= bars; ++b) {
        double s = 0.0;
        for (int j = 0; j < mf::kLearnBars; ++j) s += e[static_cast<size_t>(b + j)];
        if (s > bestSum) {
            bestSum = s;
            best = b;
        }
    }
    return best + 1;
}

// Through the chain in chunks, MPC's transport from the start (bar 1 at beat 0), playing.
template <class Each>
void run(Master& m, mfw::Audio& a, double bpm, const Master::Params& p, size_t from, size_t to, Each each) {
    for (size_t i = from; i < to; i += ef::kChunk) {
        const int n = static_cast<int>(std::min<size_t>(ef::kChunk, to - i));
        ef::Transport t;
        t.bpm = bpm;
        t.beats = static_cast<double>(i) / a.rate * bpm / 60.0;
        t.playing = t.valid = true;
        if (!each(i, t)) return;
        m.set(p, t);
        m.process(a.L.data() + i, a.R.data() + i, n);
    }
}

int master(int argc, char** argv) {
    if (argc < 4) return usage();
    const char* in = argv[2];
    const char* out = argv[3];
    const char* bpmArg = arg(argc, argv, "--bpm");
    if (!bpmArg) return usage();
    const double bpm = std::atof(bpmArg);
    if (bpm < 20.0 || bpm > 400.0) {
        std::fprintf(stderr, "--bpm %s: 20..400\n", bpmArg);
        return 2;
    }
    mfw::Audio src;
    if (!load(in, src)) return 1;

    static const char* const targetNames[] = {"YouTube", "Loud", "Live"};
    static const char* const styleNames[] = {"Warm", "Neutral", "Bright"};
    const int target = pick(arg(argc, argv, "--target"), targetNames, mf::kNumTargets, 0);
    const int style = pick(arg(argc, argv, "--style"), styleNames, mf::kNumStyles, 1);
    const float amount = arg(argc, argv, "--amount") ? static_cast<float>(std::atof(arg(argc, argv, "--amount"))) / 100.0f : 1.0f;
    const float trim = arg(argc, argv, "--loudness") ? static_cast<float>(std::atof(arg(argc, argv, "--loudness"))) : 0.0f;

    Master::Params p;
    p.width.width = arg(argc, argv, "--width") ? static_cast<float>(std::atof(arg(argc, argv, "--width"))) / 100.0f : 1.0f;
    p.width.monoHz = arg(argc, argv, "--mono") ? static_cast<float>(std::atof(arg(argc, argv, "--mono"))) : 0.0f;
    p.ceilingDb = arg(argc, argv, "--ceiling") ? static_cast<float>(std::atof(arg(argc, argv, "--ceiling"))) : -1.0f;
    p.warmth = arg(argc, argv, "--warmth") ? static_cast<float>(std::atof(arg(argc, argv, "--warmth"))) / 100.0f : 0.0f;

    const double samplesPerBar = mf::kBeatsPerBar * 60.0 / bpm * src.rate;
    const double seconds = static_cast<double>(src.L.size()) / src.rate;
    std::printf("%s: %.1f s, %.0f bars at %.1f BPM\n", in, seconds, static_cast<double>(src.L.size()) / samplesPerBar, bpm);

    Master m;
    if (!flag(argc, argv, "--no-learn")) {
        int bar = arg(argc, argv, "--learn-bar") ? std::atoi(arg(argc, argv, "--learn-bar")) : 0;
        if (bar <= 0) {
            bar = loudestBars(src, samplesPerBar);
            std::printf("LEARN on the loudest 8 bars: %d-%d\n", bar, bar + mf::kLearnBars - 1);
        } else {
            std::printf("LEARN on bars %d-%d\n", bar, bar + mf::kLearnBars - 1);
        }
        // Pass 1: play from the bar before, press LEARN there; it starts on bar `bar`'s line.
        mfw::Audio a = src;
        const double armBeat = (bar - 1) * mf::kBeatsPerBar - 1.0;
        const size_t from = static_cast<size_t>(std::max(0.0, armBeat) * 60.0 / bpm * a.rate) / ef::kChunk * ef::kChunk;
        bool armed = false;
        run(m, a, bpm, Master::Params{}, from, a.L.size(), [&](size_t, const ef::Transport&) {
            if (!armed) {
                m.learn();
                armed = true;
            }
            return m.learnState() == Master::LearnState::Armed || m.learnState() == Master::LearnState::Measuring;
        });
        if (m.learnState() == Master::LearnState::Measuring) {   // the 8 bars ran to the file's end:
            ef::Transport t;                                     // one more look at the transport, as
            t.bpm = bpm;                                         // the next chunk would give on the Force
            t.beats = static_cast<double>(a.L.size()) / a.rate * bpm / 60.0;
            t.playing = t.valid = true;
            m.set(Master::Params{}, t);
        }
        if (m.learnState() != Master::LearnState::Done) {
            std::fprintf(stderr, "LEARN failed: %s\n", m.learnState() == Master::LearnState::Failed ? m.learnError() : "the file ended first");
            return 1;
        }
        const mf::Measurement& me = m.measurement();
        const mf::BandFit fit = mf::fitBands(me.bandDb);
        std::printf("  measured %.1f LUFS, true peak %.1f dBTP, PLR %.1f dB; slope %.2f dB/oct, sub %+.1f, air %+.1f\n", me.loudness,
                    me.truePeakDb, me.plr(), fit.slope, fit.subDev, fit.airDev);
        printBands(me.bandDb);

        const mf::Settings s = mf::derive(me, target, style, amount, p.ceilingDb);
        p.tone = s.tone;
        p.glueDb = s.glueDb;
        p.glueThresholdDb = s.glueDb > 0.0f ? mf::glueThresholdFor(me, s.glueDb) : 0.0f;
        p.gainDb = s.gainDb + trim;
        std::printf("LEARNED (%s, %s, %.0f%%): tilt %+.1f · low %+.1f · air %+.1f · glue %.1f dB (threshold %.1f) · gain %+.1f dB\n",
                    targetNames[target], styleNames[style], amount * 100.0f, s.tone.tiltDb, s.tone.lowDb, s.tone.airDb, s.glueDb,
                    p.glueThresholdDb, p.gainDb);
        if (s.shortDb > 0.0f) std::printf("  %.1f dB short of the target: the section is too quiet for the gain's +18 dB\n", s.shortDb);
    } else {
        p.gainDb = trim;
    }

    // Pass 2: the whole file, from a fresh chain, with the settings; the latency's worth of silence
    // after it so the tail comes out, then the first kLatency samples dropped.
    mfw::Audio a = src;
    a.L.resize(src.L.size() + Master::kLatency, 0.0f);
    a.R.resize(src.R.size() + Master::kLatency, 0.0f);
    Master chain;
    float maxReduction = 0.0f;
    run(chain, a, bpm, p, 0, a.L.size(), [&](size_t i, const ef::Transport&) {
        if (i % 4410 < ef::kChunk) maxReduction = std::max(maxReduction, chain.takeReductionDb());
        return true;
    });
    maxReduction = std::max(maxReduction, chain.takeReductionDb());
    a.L.erase(a.L.begin(), a.L.begin() + Master::kLatency);
    a.R.erase(a.R.begin(), a.R.begin() + Master::kLatency);

    mf::Loudness check;
    check.process(a.L.data(), a.R.data(), static_cast<int>(a.L.size()));
    std::printf("OUT: %.1f LUFS (target %.1f), true peak %.2f dBTP, deepest reduction %.1f dB\n", check.integrated(),
                mf::kTargets[target].lufs, check.truePeakDb(), maxReduction);

    std::string err;
    if (!mfw::write24(out, a, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::printf("wrote %s\n", out);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "analyze")) return analyze(argv[2]);
    if (argc >= 2 && !std::strcmp(argv[1], "master")) return master(argc, argv);
    return usage();
}
