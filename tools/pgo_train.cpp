// The profile-guided build's trainer (make arm-plugin with PGO): an instrumented copy of the plugin is
// linked in and plays the reference mix (tools/reference_mix.h) through VSTPluginMain under qemu-arm,
// the way MPC drives an insert: at rest, through a LEARN, with the learned knobs, every option of the
// choices, warmth and width swept, then every factory preset. The profile only steers the compiler
// (which paths are hot); what the trainer leaves out is still optimised as usual
// (-fprofile-partial-training).
#include "../plugin/vst2.h"
#include "factory_presets.h"
#include "param_ids.h"
#include "reference_mix.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback);

namespace {

using namespace mf;

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == vst::audioMasterVersion) return 2400;
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

std::vector<float> g_L, g_R;   // the reference mix
size_t g_at = 0;

// Plays `seconds` of the reference mix through the plugin at `gain`, in place, 128 frames at a time.
void play(AEffect* e, double seconds, float gain = 1.0f) {
    float L[128], R[128];
    float* io[2] = {L, R};
    const int blocks = static_cast<int>(seconds * 44100.0 / 128.0);
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < 128; ++i) {
            L[i] = gain * g_L[g_at];
            R[i] = gain * g_R[g_at];
            g_at = (g_at + 1) % g_L.size();
        }
        e->processReplacing(e, io, io, 128);
        g_time.ppqPos += 128.0 / 44100.0 * g_time.tempo / 60.0;
        e->getParameter(e, P_STATUS);   // MPC's UI side: a LEARN's measurement is taken up
    }
}

AEffect* fresh() {
    AEffect* e = VSTPluginMain(master);
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    return e;
}

void done(AEffect* e) {
    char b[256];
    for (int i = 0; i < P_COUNT; ++i) e->dispatcher(e, vst::effGetParamDisplay, i, 0, b, 0.0f);
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
}

void press(AEffect* e, int id) { e->setParameter(e, id, e->getParameter(e, id) > 0.5f ? 0.0f : 1.0f); }

} // namespace

int main() {
    efl::referenceMix(g_L, g_R);
    g_time.sampleRate = 44100.0;
    g_time.tempo = 240.0;   // 8 bars in 8 s: a LEARN is quick to train
    g_time.timeSigNumerator = g_time.timeSigDenominator = 4;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid | vst::kVstTransportPlaying;

    {   // at rest, then hot: the limiter alone
        AEffect* e = fresh();
        play(e, 2.0);
        play(e, 2.0, 4.0f);
        done(e);
    }
    {   // a LEARN on each target, the learned chain, every choice, the knobs swept
        AEffect* e = fresh();
        press(e, P_LEARN);
        play(e, 9.0);
        for (int t = 0; t < PARAM_INFO[P_TARGET].nopts; ++t) {
            e->setParameter(e, P_TARGET, static_cast<float>(t) / static_cast<float>(PARAM_INFO[P_TARGET].nopts - 1));
            for (int s = 0; s < PARAM_INFO[P_STYLE].nopts; ++s) {
                e->setParameter(e, P_STYLE, static_cast<float>(s) / static_cast<float>(PARAM_INFO[P_STYLE].nopts - 1));
                play(e, 0.5);
            }
        }
        for (int m = 0; m < PARAM_INFO[P_MONO].nopts; ++m) {
            e->setParameter(e, P_MONO, static_cast<float>(m) / static_cast<float>(PARAM_INFO[P_MONO].nopts - 1));
            play(e, 0.5);
        }
        for (int k = 0; k <= 8; ++k) {
            const float v = static_cast<float>(k) / 8.0f;
            e->setParameter(e, P_WARMTH, v);
            e->setParameter(e, P_WIDTH, v);
            e->setParameter(e, P_GLUE, v);
            e->setParameter(e, P_GAIN, 0.4f + 0.6f * v);
            play(e, 0.4);
        }
        done(e);
    }
    for (int p = 0; p < kNumFactoryPresets; ++p) {   // every factory preset, after a LEARN
        AEffect* e = fresh();
        press(e, P_LEARN);
        play(e, 8.5);
        for (int k = 0; k <= p; ++k) press(e, P_PRESET_NEXT);
        play(e, 1.0);
        done(e);
    }
    std::printf("pgo_train: done\n");
    return 0;
}
