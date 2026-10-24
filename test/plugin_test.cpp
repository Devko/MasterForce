// The test suite: every stage of the chain measured on its own (test/*_test.cpp), then the whole
// plugin driven through its VST2 entry points the way MPC drives an insert effect (128-frame blocks
// in place, 0..1 params, the Force's taps and turns, MPC's transport). Built with ASan/UBSan by
// `make test`, for the Force's CPU under qemu by `make test-arm`.
#include "host.h"
#include "measure.h"
#include "../plugin/state.h"
#include "../plugin/surface.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

int eft::g_fail = 0, eft::g_pass = 0;
long long eft::g_msPerEvent = 1000;

namespace eft {

intptr_t hostMaster(AEffect* e, int32_t op, int32_t index, intptr_t, void*, float opt) {
    HostLog* log = e ? static_cast<HostLog*>(e->user) : nullptr;
    if (op == vst::audioMasterVersion) return 2400;
    if (!log) return 0;
    if (op == vst::audioMasterUpdateDisplay) ++log->updates;
    if (op == vst::audioMasterAutomate) {
        log->automated[index] = opt;
        ++log->automateCount[index];
    }
    if (op == vst::audioMasterGetTime) return log->noTime ? 0 : reinterpret_cast<intptr_t>(&log->time);
    return 0;
}

std::string fixtureDir() {
    static const std::string dir = [] {
        char tmpl[] = "/tmp/mftest.XXXXXX";
        const char* d = mkdtemp(tmpl);
        return std::string(d ? d : "/tmp/mftest");
    }();
    return dir;
}

namespace {

using namespace mf;
constexpr int kBar = 2 * 44100;   // at the host's 120 BPM

bool starts(const std::string& s, const char* with) { return s.compare(0, std::strlen(with), with) == 0; }

// A beat at 120 BPM for `bars` bars (test/measure.h), its peaks about `amp`.
void beatBars(Buf& L, Buf& R, int bars, float amp) { beat(L, R, bars * kBar, amp); }

// LEARN on the first 8 bars of a beat, played from the song's start: the host's LEARN press, MPC
// playing, then a getter (MPC's UI thread) takes the measurement up.
void learnBeat(Host& h, float amp = 0.3f) {
    Buf L, R;
    beatBars(L, R, 9, amp);
    h.press(P_LEARN);
    h.play(true);
    h.run(L, R);
    h.get(P_STATUS);
}

void testBasics() {
    std::printf("== the plugin as MPC sees it\n");
    Host h;
    AEffect* e = h.e;
    CHECK(e->magic == vst::kMagic && e->numInputs == 2 && e->numOutputs == 2);
    CHECK((e->flags & vst::effFlagsCanReplacing) && (e->flags & vst::effFlagsProgramChunks) && !(e->flags & vst::effFlagsIsSynth));
    CHECK(e->initialDelay == 0);   // the limiter's 64 samples are not reported: on the master nothing is out of line
    CHECK(e->numParams == P_COUNT && e->uniqueID == vst::fourcc("MsFc"));
    CHECK(h.op(vst::effGetPlugCategory) == vst::kPlugCategEffect);
    char b[64] = {};
    h.op(vst::effGetEffectName, 0, 0, b);
    CHECK(std::string(b) == "MasterForce");
    CHECK(h.op(vst::effCanDo, 0, 0, const_cast<char*>("plugAsChannelInsert")) == 1);
    CHECK(h.op(vst::effCanDo, 0, 0, const_cast<char*>("receiveVstMidiEvent")) == -1);
    CHECK(h.op(vst::effCanDo, 0, 0, const_cast<char*>("bypass")) == -1);
    VstSpeakerArrangementHead st{1, 2}, mono{0, 1};
    CHECK(h.op(vst::effSetSpeakerArrangement, 0, reinterpret_cast<intptr_t>(&st), &st) == 1);
    CHECK(h.op(vst::effSetSpeakerArrangement, 0, reinterpret_cast<intptr_t>(&mono), &st) == 0);
    // MPC sets parameter 0 when it loads a plugin: a readout ignores it.
    h.setN(P_STATUS, 0.7f);
    CHECK(h.get(P_STATUS) == 0.0f);
    CHECK(h.op(vst::effCanBeAutomated, P_TARGET) == 1 && h.op(vst::effCanBeAutomated, P_GAIN) == 1);
    CHECK(h.op(vst::effCanBeAutomated, P_LEARN) == 0 && h.op(vst::effCanBeAutomated, P_LEARN_INFO) == 0);
    CHECK(h.op(vst::effGetTailSize) >= Master::kLatency);
    for (int i = 0; i < P_COUNT; ++i) CHECK(!h.name(i).empty() && h.name(i).size() <= 24);
    // The values as the knobs show them.
    CHECK(h.display(P_CEILING) == "-1.0 dBTP");
    CHECK(h.display(P_GAIN) == "+0.0 dB");
    CHECK(h.display(P_TARGET) == "YouTube" && h.display(P_MONO) == "Off" && h.display(P_AMOUNT) == "100%");
    CHECK(starts(h.display(P_LEARN_INFO), "PRESS LEARN"));
}

void testPassThrough() {
    std::printf("== at rest: the input, 64 samples late, bit for bit\n");
    const int n = kBlocksPerSec * kBlock;
    const Buf l = whiteNoise(n, 0.3f, 3), r = whiteNoise(n, 0.3f, 4);
    auto delayed = [&](const Buf& out, const Buf& in) {
        for (size_t i = 0; i < out.size(); ++i)
            if (out[i] != (i < static_cast<size_t>(Master::kLatency) ? 0.0f : in[i - Master::kLatency])) return false;
        return true;
    };
    {
        Host h;
        h.run(l, r);
        CHECK(delayed(h.L, l) && delayed(h.R, r));
    }
    {
        Host h;
        h.inPlace = false;   // a host with separate buffers
        h.run(l, r, 100);
        CHECK(delayed(h.L, l) && delayed(h.R, r));
    }
    {   // the legacy accumulating process() adds to what is there
        Host h;
        Buf a = l, bb = r, oL(l.size(), 1.0f), oR(l.size(), 1.0f);
        float* in[2] = {a.data(), bb.data()};
        float* out[2] = {oL.data(), oR.data()};
        h.e->process(h.e, in, out, static_cast<int32_t>(l.size()));
        float worst = 0.0f;
        for (size_t i = Master::kLatency; i < l.size(); ++i) worst = std::max(worst, std::fabs(oL[i] - 1.0f - l[i - Master::kLatency]));
        CHECK(worst < 1e-6f);
    }
    {   // a NaN or infinity from the host never leaves the plugin
        Host h;
        Buf x = l;
        x[1000] = std::nanf("");
        x[2000] = INFINITY;
        h.run(x, r);
        CHECK(h.finite && h.L[1000 + Master::kLatency] == 0.0f && h.L[2000 + Master::kLatency] == 0.0f);
    }
    {   // no input buffers: silence, not a crash
        Host h;
        float oL[kBlock], oR[kBlock];
        float* out[2] = {oL, oR};
        h.e->processReplacing(h.e, nullptr, out, kBlock);
        CHECK(oL[0] == 0.0f && oR[kBlock - 1] == 0.0f);
    }
    {   // hot: nothing over the ceiling, whatever comes in
        Host h;
        h.run(whiteNoise(n, 4.0f, 5), whiteNoise(n, 4.0f, 6));
        CHECK(h.finite && peak(h.L) <= 0.8913f && peak(h.R) <= 0.8913f);
    }
}

void testLearn() {
    std::printf("== LEARN: 8 bars, the knobs, the project\n");
    Host h;
    CHECK(h.display(P_LEARN_INFO).find("LOUDEST 8 BARS") != std::string::npos);
    // Pressed while MPC is stopped: it waits for play.
    h.press(P_LEARN);
    h.blocks(4);
    CHECK(starts(h.display(P_LEARN_INFO), "LEARN: IT STARTS"));
    Buf L, R;
    beatBars(L, R, 9, 0.3f);
    h.play(true);
    const int part = 3 * kBar + kBar / 2;   // half way through the 4th bar
    h.run(Buf(L.begin(), L.begin() + part), Buf(R.begin(), R.begin() + part));
    CHECK(h.display(P_LEARN_INFO) == "LEARNING: BAR 4 OF 8");
    h.run(Buf(L.begin() + part, L.end()), Buf(R.begin() + part, R.end()));
    h.get(P_STATUS);   // MPC's UI side: the measurement is taken up
    CHECK(starts(h.display(P_LEARN_INFO), "LEARNED"));
    const float gain = h.value(P_GAIN);
    CHECK(gain > 3.0f && gain < 18.0f);   // a beat peaking at -10 dBFS is far under -14 LUFS
    CHECK(h.value(P_GLUE) > 0.0f);
    h.blocks(8);
    CHECK(h.log.automated.count(P_GAIN) && std::fabs(h.log.automated[P_GAIN] - h.get(P_GAIN)) < 1e-6f);   // MPC was told
    const std::string chunk = h.chunk();
    CHECK(chunk.find("\nlearn=1;") != std::string::npos);

    // TARGET Loud: the gain derived again from the measurement, 4 dB more (Loud is 4 LU louder).
    h.option(P_TARGET, "Loud");
    CHECK(std::fabs(h.value(P_GAIN) - std::min(gain + 4.0f, 18.0f)) < 0.05f);
    h.option(P_TARGET, "YouTube");
    CHECK(std::fabs(h.value(P_GAIN) - gain) < 0.05f);
    // AMOUNT 0: no tone, no glue; the gain stays (it goes to the target whatever AMOUNT says).
    h.set(P_AMOUNT, 0.0f);
    CHECK(h.value(P_TILT) == 0.0f && h.value(P_LOW) == 0.0f && h.value(P_AIR) == 0.0f && h.value(P_GLUE) == 0.0f);
    h.set(P_AMOUNT, 1.0f);

    // The project: a new instance with this state has the measurement, and the knobs as saved.
    {
        Host g;
        CHECK(g.load(chunk) == 1);
        CHECK(std::fabs(g.value(P_GAIN) - gain) < 0.01f);
        CHECK(starts(g.display(P_LEARN_INFO), "LEARNED"));
        g.option(P_TARGET, "Loud");
        CHECK(std::fabs(g.value(P_GAIN) - std::min(gain + 4.0f, 18.0f)) < 0.05f);
        // A project without a measurement takes it away.
        mf::Measurement none;
        std::string bare = chunk.substr(0, chunk.find("learn="));
        CHECK(g.load(bare) == 1);
        CHECK(starts(g.display(P_LEARN_INFO), "PRESS LEARN"));
    }
    // A preset says how to master, the measurement is the song's: INIT keeps it and derives again.
    h.option(P_TARGET, "Loud");
    h.press(P_PRE_INIT);
    CHECK(h.display(P_TARGET) == "YouTube");
    CHECK(std::fabs(h.value(P_GAIN) - gain) < 0.05f);
    // A preset saved now holds no measurement and none of the knobs LEARN sets.
    h.press(P_PRE_SAVE);
    std::string saved;
    for (const auto& f : std::filesystem::recursive_directory_iterator(fixtureDir() + "/presets"))
        if (f.path().extension() == ".mfp") {
            std::ifstream in(f.path());
            std::stringstream ss;
            ss << in.rdbuf();
            saved = ss.str();
        }
    CHECK(starts(saved, "masterforce 1") && saved.find("target=") != std::string::npos);
    CHECK(saved.find("gain=") == std::string::npos && saved.find("tilt=") == std::string::npos &&
          saved.find("learn=") == std::string::npos);

    // LEARN again, and a press while it measures cancels it.
    h.press(P_LEARN);
    h.run(Buf(L.begin(), L.begin() + kBar), Buf(R.begin(), R.begin() + kBar));
    CHECK(starts(h.display(P_LEARN_INFO), "LEARNING"));
    h.press(P_LEARN);
    h.blocks(2);
    CHECK(starts(h.display(P_LEARN_INFO), "LEARNED"));   // back to what it had
}

void testLearnFails() {
    std::printf("== LEARN: stopped, too quiet\n");
    {
        Host h;
        Buf L, R;
        beatBars(L, R, 3, 0.3f);
        h.press(P_LEARN);
        h.play(true);
        h.run(L, R);
        h.play(false);
        h.blocks(4);
        CHECK(starts(h.display(P_LEARN_INFO), "LEARN STOPPED"));
        CHECK(h.get(P_GAIN) == PARAM_INFO[P_GAIN].def);   // nothing set
    }
    {
        Host h;
        h.press(P_LEARN);
        h.play(true);
        h.silence(9 * kBar);
        h.get(P_STATUS);
        CHECK(starts(h.display(P_LEARN_INFO), "LEARN: TOO QUIET"));
        CHECK(h.get(P_GAIN) == PARAM_INFO[P_GAIN].def);
    }
}

void testLearnOnAForce() {
    std::printf("== LEARN on a Force: a 4-bar loop, the position stepping back every other block\n");
    Host h;
    Buf L, R;
    beatBars(L, R, 9, 0.3f);
    h.press(P_LEARN);
    h.play(true);
    const double beatsPerBlock = kBlock * 120.0 / 60.0 / 44100.0;
    for (size_t pos = 0, b = 0; pos + kBlock <= L.size(); pos += kBlock, ++b) {
        h.run(Buf(L.begin() + static_cast<long>(pos), L.begin() + static_cast<long>(pos + kBlock)),
              Buf(R.begin() + static_cast<long>(pos), R.begin() + static_cast<long>(pos + kBlock)));
        if (b % 2 == 1) h.log.time.ppqPos -= beatsPerBlock;           // a stale position from the other worker
        if (h.log.time.ppqPos >= 16.0) h.log.time.ppqPos -= 16.0;    // the sequence loops every 4 bars
    }
    h.get(P_STATUS);
    CHECK(starts(h.display(P_LEARN_INFO), "LEARNED"));
    CHECK(h.value(P_GAIN) > 3.0f);
}

void testMeters() {
    std::printf("== the meters' line\n");
    Host h;
    CHECK(starts(h.display(P_STATUS), "-- LUFS"));
    Buf L, R;
    beatBars(L, R, 3, 0.3f);
    h.play(true);
    const int before = h.log.updates;
    h.run(L, R);
    const std::string s = h.display(P_STATUS);
    CHECK(s.find(" LUFS   SONG -") != std::string::npos && s.find("PEAK -") != std::string::npos);
    CHECK(s.find("CPU ") != std::string::npos);
    // A few times a second while it plays (the meters at most 4, the CPU meter 2), never every block: 6 s.
    const int told = h.log.updates - before;
    CHECK(told > 4 && told <= 6 * (4 + 2) + 2);
}

void testLearnedSound() {
    std::printf("== LEARN's master: near its aim, under the ceiling\n");
    Host h;
    learnBeat(h, 0.6f);
    Buf L, R;
    beatBars(L, R, 9, 0.6f);
    h.run(L, R);
    const Buf oL(h.L.begin() + kBar, h.L.end()), oR(h.R.begin() + kBar, h.R.end());
    Loudness m;
    m.process(oL.data(), oR.data(), static_cast<int>(oL.size()));
    CHECK(std::fabs(m.integrated() - (kTargets[0].lufs + kAboveTarget)) < 1.5f);
    CHECK(dbOf(truePeakRef(oL, oR)) <= -1.0 + 0.1);
    CHECK(h.finite);
}

void testSuspend() {
    std::printf("== suspend and resume (Stop, the slot's ON button)\n");
    const Buf in = whiteNoise(4 * kBlock, 0.3f, 7);
    {   // Stop (a short suspend): the delayed samples come out after it
        Host h;
        h.run(in);
        {
            Turn quick(5);
            h.op(vst::effMainsChanged, 0, 0);
            h.op(vst::effMainsChanged, 0, 1);
        }
        h.silence(kBlock);
        CHECK(h.L[0] == in[in.size() - Master::kLatency]);
    }
    {   // the slot switched off for a while: cleared, nothing old comes back
        Host h;
        h.run(in);
        h.op(vst::effMainsChanged, 0, 0);
        h.op(vst::effMainsChanged, 0, 1);
        h.silence(kBlock);
        CHECK(peak(h.L) == 0.0f);
    }
}

void testPresets() {
    std::printf("== the factory presets\n");
    Host h;
    const std::string first = h.display(P_PRESET);
    std::set<std::string> names;
    for (int k = 0; k < 20; ++k) {
        names.insert(h.display(P_PRESET));
        h.press(P_PRESET_NEXT);
        h.blocks(1);
    }
    CHECK(names.size() >= 15);   // every factory preset, one press each
    CHECK(h.finite);
    h.press(P_PRE_INIT);
    CHECK(h.display(P_PRESET).find("Init") != std::string::npos);
    CHECK(first != h.display(P_PRESET) || first.find("Init") != std::string::npos);
}

void testStress() {
    std::printf("== everything at once, at random\n");
    Host h;
    uint32_t seed = 12345;
    auto rnd = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    Buf L, R;
    beatBars(L, R, 1, 0.9f);
    h.play(true);
    for (int round = 0; round < 40; ++round) {
        for (int k = 0; k < 6; ++k) {
            const int id = static_cast<int>(rnd() * P_COUNT) % P_COUNT;
            if (PARAM_INFO[id].kind == Kind::Synth) h.setN(id, rnd());
        }
        if (round % 10 == 3) h.press(P_LEARN);
        h.run(Buf(L.begin(), L.begin() + kBar / 8), Buf(R.begin(), R.begin() + kBar / 8));
        CHECK(h.finite && peak(h.L) <= 1.0f && peak(h.R) <= 1.0f);
    }
}

} // namespace
} // namespace eft

int main() {
    using namespace eft;
    const std::string root = fixtureDir();
    std::filesystem::create_directories(root + "/presets");
    std::filesystem::create_directories(root + "/data");
    setenv("MF_PRESET_ROOTS", (root + "/presets").c_str(), 1);
    setenv("MF_DATA_DIR", (root + "/data").c_str(), 1);
    setenv("MF_FIXED_SEED", "1", 1);
    // Host events a second apart unless a test says otherwise (eft::Turn): stepping never
    // mistakes two of them for one turn, whatever the machine's speed (and qemu's).
    mf::Surface::clock = [] {
        static long long t = 0;
        return t += g_msPerEvent;
    };

    std::printf("== the chain\n");
    limiterTests();
    loudnessTests();
    stageTests();
    learnTests();
    masterTests();

    testBasics();
    testPassThrough();
    testLearn();
    testLearnFails();
    testLearnOnAForce();
    testMeters();
    testLearnedSound();
    testSuspend();
    testPresets();
    testStress();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::printf("%s: %d passed, %d failed\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
