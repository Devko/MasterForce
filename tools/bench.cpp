// CPU bench for the built plugin: dlopen()s the .so like MPC does, feeds noise at 44.1 kHz in
// 128-frame blocks, in place, and times every processReplacing call with the thread's own CPU clock.
// Same verdict rule as sd88me's tools/bench.sh (percent of the 2902 us block): PASS p99 <= 15% and
// max <= 50%, WARN p99 <= 35% and max <= 80%, else FAIL.
//
//   mfbench <plugin.so> [-s seconds] [-c cpu]
//
// Cases: at rest (the end of the chain: the limiter and the meters); LEARN measuring; the learned
// chain driving the clipper; and everything at once (warmth, width and mono bass on top, while LEARN
// measures): the worst a setting can reach. Hermetic: the plugin reads no user folders, saves nothing.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../plugin/vst2.h"
#include "param_ids.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <sched.h>
#include <string>
#include <vector>

namespace {

using namespace mf;

constexpr int kBlock = 128;
constexpr double kBudgetUs = kBlock * 1e6 / 44100.0;   // 2902 us

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == vst::audioMasterVersion) return 2400;
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

double cpuUs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) * 1e-3;
}

float norm(int id, float v) {
    const ParamSpec& s = PARAM_SPECS[id];
    return std::clamp(s.hi > s.lo ? (v - s.lo) / (s.hi - s.lo) : 0.0f, 0.0f, 1.0f);
}

struct Result {
    double avg, p99, max;
};

enum Case { kRest, kLearning, kLearned, kEverything };

Result runCase(void* lib, int seconds, const char* name, Case c) {
    auto entry = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(dlsym(lib, "VSTPluginMain"));
    AEffect* e = entry ? entry(master) : nullptr;
    if (!e) {
        std::fprintf(stderr, "no plugin\n");
        std::exit(1);
    }
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    auto set = [e](int id, float v) { e->setParameter(e, id, norm(id, v)); };
    if (c == kLearned || c == kEverything) {
        set(P_TILT, 2.0f);
        set(P_LOW, -1.5f);
        set(P_AIR, 1.5f);
        set(P_GLUE, 3.0f);
        set(P_GAIN, 9.0f);   // the clipper works
    }
    if (c == kEverything) {
        set(P_WARMTH, 1.0f);
        set(P_WIDTH, 1.5f);
        e->setParameter(e, P_MONO, 2.0f / 3.0f);   // 120 Hz
    }
    if (c == kLearning || c == kEverything) e->setParameter(e, P_LEARN, 1.0f);
    std::vector<float> L(kBlock), R(kBlock);
    float* io[2] = {L.data(), R.data()};
    uint32_t seed = 12345u;
    auto fill = [&] {   // noise around -12 dBFS, a little different per side
        for (int i = 0; i < kBlock; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const float x = static_cast<float>(static_cast<int32_t>(seed)) * (0.25f / 2147483648.0f);
            L[static_cast<size_t>(i)] = x;
            R[static_cast<size_t>(i)] = 0.7f * x + 0.05f;
        }
    };
    for (int b = 0; b < 64; ++b) {   // the settings land, the glue and warmth wake
        fill();
        e->processReplacing(e, io, io, kBlock);
        g_time.ppqPos += kBlock / 44100.0 * g_time.tempo / 60.0;
    }
    const int blocks = seconds * 44100 / kBlock;
    std::vector<double> t(static_cast<size_t>(blocks));
    for (int b = 0; b < blocks; ++b) {
        fill();
        const double t0 = cpuUs();
        e->processReplacing(e, io, io, kBlock);
        t[static_cast<size_t>(b)] = cpuUs() - t0;
        g_time.ppqPos += kBlock / 44100.0 * g_time.tempo / 60.0;
    }
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
    std::vector<double> s = t;
    std::sort(s.begin(), s.end());
    double sum = 0.0;
    for (double v : t) sum += v;
    const Result r{100.0 * sum / blocks / kBudgetUs, 100.0 * s[static_cast<size_t>(blocks * 0.99)] / kBudgetUs,
                   100.0 * s.back() / kBudgetUs};
    const char* verdict = r.p99 <= 15.0 && r.max <= 50.0 ? "PASS" : r.p99 <= 35.0 && r.max <= 80.0 ? "WARN" : "FAIL";
    std::printf("  %-38s avg %5.2f%%  p99 %5.2f%%  max %5.2f%%  %s\n", name, r.avg, r.p99, r.max, verdict);
    return r;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <plugin.so> [-s seconds] [-c cpu]\n", argv[0]);
        return 2;
    }
    int seconds = 3, cpu = 1;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "-s")) seconds = std::max(1, std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "-c")) cpu = std::atoi(argv[i + 1]);
    }
    if (cpu >= 0) {   // -c -1: don't pin (x86 runs, CI)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof set, &set) != 0) std::printf("(could not pin to cpu %d)\n", cpu);
    }
    // Hermetic: no user folders, nothing saved.
    setenv("MF_PRESET_ROOTS", "/nonexistent-mfbench", 1);
    setenv("MF_DATA_DIR", "", 1);
    g_time.sampleRate = 44100.0;
    g_time.tempo = 120.0;
    g_time.timeSigNumerator = g_time.timeSigDenominator = 4;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid | vst::kVstTransportPlaying;

    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    std::printf("%s, %d s per case\n", argv[1], seconds);
    bool fail = false;
    const auto check = [&fail](const Result& r) { fail = fail || !(r.p99 <= 35.0 && r.max <= 80.0); };
    check(runCase(lib, seconds, "at rest: limiter and meters", kRest));
    check(runCase(lib, seconds, "LEARN measuring", kLearning));
    check(runCase(lib, seconds, "learned: tone, glue, gain into clipper", kLearned));
    check(runCase(lib, seconds, "everything, while LEARN measures", kEverything));
    dlclose(lib);
    return fail ? 1 : 0;
}
