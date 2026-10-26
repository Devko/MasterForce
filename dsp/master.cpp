#include "master.h"

#include <cmath>

namespace mf {

namespace {

constexpr double kWrapBeats = 1.0;   // the song position falling back more than this: a loop starting over
constexpr double kOnLineBeats = 0.01;   // playing from a stop this close to a bar line starts on it
constexpr int kGlueWarmChunks = 96;  // 70 ms: the glue's detector settles (its attack is 30 ms) unheard
constexpr float kGlueFadeStep = 1.0f / 16.0f;   // in and out over 16 chunks, 12 ms

} // namespace

Master::Master() { reset(); }

void Master::reset() {
    tone_.reset();
    glue_.reset();
    glueOn_ = glueOffNext_ = wasPlaying_ = false;
    glueWarm_ = 0;
    glueMix_ = 0.0f;
    gain_.jump(1.0f);
    warmth_.reset();
    warmOn_ = warmOffNext_ = false;
    warmMix_ = 0.0f;
    for (int c = 0; c < 2; ++c) std::fill(warmHist_[c], warmHist_[c] + kWarmHistory, 0.0f);
    warmHistPos_ = 0;
    width_.reset();
    clip_.reset();
    limiter_.reset();
    meter_.reset();
    clipPeak_ = 0.0f;
    gainDb_ = 0.0f;
    gainLin_ = 1.0f;
    clipCeilDb_ = -1.0f;
    clipLevel_ = std::pow(10.0f, (clipCeilDb_ + kClipOverDb) / 20.0f);
    fresh_ = true;
}

void Master::set(const Params& p, const ef::Transport& t) {
    tone_.set(p.tone, t);

    // Glue: EffectForce's Comp, off at 0 and then not processed at all. Woken, it starts cleared and
    // runs unheard (mix 0 is its input bit for bit) until its detector has settled, so its makeup
    // doesn't sound before its reduction does; then it fades in. Put to sleep, it fades out first.
    if (p.glueDb > 0.0f) {
        if (!glueOn_) {
            glue_.reset();
            glueOn_ = true;
            glueWarm_ = kGlueWarmChunks;
            glueMix_ = 0.0f;
        }
        if (glueWarm_ > 0) --glueWarm_;
        else glueMix_ = std::min(1.0f, glueMix_ + kGlueFadeStep);
        ef::Comp::Params c;
        c.thresholdDb = p.glueThresholdDb;
        c.ratio = kGlueRatio;
        c.attackMs = kGlueAttackMs;
        c.releaseMs = kGlueReleaseMs;
        c.kneeDb = kGlueKneeDb;
        c.scLowCutHz = kGlueScHz;
        c.makeupDb = p.glueDb;
        c.mix = glueMix_;
        glue_.set(c, t);
        glueLast_ = c;
        glueOffNext_ = false;
    } else if (glueOn_) {
        if (glueOffNext_) {
            glueOn_ = glueOffNext_ = false;
        } else {
            glueMix_ = std::max(0.0f, glueMix_ - kGlueFadeStep);
            glueLast_.mix = glueMix_;
            glue_.set(glueLast_, t);
            glueOffNext_ = glueMix_ == 0.0f;   // this chunk glides to dry; the next isn't processed
        }
    }

    // The song's meter starts again when MPC starts.
    if (t.playing && !wasPlaying_) meter_.reset();
    wasPlaying_ = t.playing;

    const float gainDb = ef::clampParam(p.gainDb, -12.0f, 18.0f, 0.0f);
    if (!sameBits(gainDb, gainDb_)) {   // libm only when the knob moved, not every chunk
        gainDb_ = gainDb;
        gainLin_ = gainDb == 0.0f ? 1.0f : std::pow(10.0f, gainDb / 20.0f);
    }
    if (fresh_) gain_.jump(gainLin_);
    else gain_.to(gainLin_, ef::kChunk);

    // Warmth: EffectForce's Drive, Tube (a triode's curve: even harmonics at every level), its gain
    // compensation keeping the level. WARMTH blends it in and drives it together, a touch darker:
    // up to half way a little density and a soft second harmonic, at full plainly saturated. Off at 0
    // and then not processed at all, but the chain keeps the last kWarmHistory samples it would have
    // had: woken, it plays them first (into nothing), so its halfbands and its dry path start in step
    // with the music instead of from silence (they would ring for a few hundred samples). Its mix
    // moves at most kGlueFadeStep a chunk (12 ms end to end). Put to sleep, it fades to dry (mix 0 is
    // its input bit for bit) and stops a chunk later.
    const float warm = ef::clampParam(p.warmth, 0.0f, 1.0f, 0.0f);
    if (warm > 0.0f || warmOn_) {
        if (!warmOn_) {
            warmth_.reset();
            warmOn_ = true;
            warmMix_ = 0.0f;
            warmLast_.type = ef::Drive::Tube;
            warmLast_.bias = 0.0f;
            warmLast_.outDb = 0.0f;
            warmLast_.mix = 0.0f;
            warmLast_.driveDb = kWarmDriveDb * warm;
            warmLast_.tone = -0.1f * warm;
            warmth_.set(warmLast_, t);   // the first set after a reset lands at once: dry
            float l[ef::kChunk], r[ef::kChunk];
            for (int j = 0; j < kWarmHistory; j += ef::kChunk) {
                for (int i = 0; i < ef::kChunk; ++i) {
                    const int at = (warmHistPos_ + j + i) & (kWarmHistory - 1);
                    l[i] = warmHist_[0][at];
                    r[i] = warmHist_[1][at];
                }
                warmth_.set(warmLast_, t);
                warmth_.process(l, r, ef::kChunk);
            }
        }
        if (warm > 0.0f) {   // drive and tone follow the knob; at 0 they stay as they were while it fades
            warmLast_.driveDb = kWarmDriveDb * warm;
            warmLast_.tone = -0.1f * warm;
        }
        warmMix_ = warmMix_ < warm ? std::min(warm, warmMix_ + kGlueFadeStep) : std::max(warm, warmMix_ - kGlueFadeStep);
        warmLast_.mix = warmMix_;
        if (warm == 0.0f && warmMix_ == 0.0f && warmOffNext_) {
            warmOn_ = warmOffNext_ = false;
        } else {
            warmth_.set(warmLast_, t);
            warmOffNext_ = warm == 0.0f && warmMix_ == 0.0f;   // this chunk glides to dry; the next isn't processed
        }
    }

    width_.set(p.width, t);

    Limiter::Params lp;
    lp.ceilingDb = p.ceilingDb;
    limiter_.set(lp);
    const float ceilDb = ef::clampParam(p.ceilingDb, -12.0f, 0.0f, -1.0f);
    if (!sameBits(ceilDb, clipCeilDb_)) {
        clipCeilDb_ = ceilDb;
        clipLevel_ = std::pow(10.0f, (ceilDb + kClipOverDb) / 20.0f);
    }
    clip_.set(clipLevel_, gainDb > 0.0f);
    fresh_ = false;

    // LEARN: armed, it waits for MPC to play and starts on a bar line: playing from a stop on one, the
    // first chunk past one, or where a loop starts over (the song position falling back more than a
    // beat). From there it counts its 8 bars itself, from the tempo and the samples it measured: a
    // sequence or clip looping, a locate, or MPC's song position stepping unevenly from block to block
    // (its two audio workers take turns) don't disturb it. Only stopping does.
    bpm_ = t.bpm;
    switch (learn_) {
    case LearnState::Armed: {
        if (!t.playing) {
            haveLast_ = false;   // waits for play
            break;
        }
        bool start = !t.valid;   // no song position: no grid to wait for
        if (t.valid) {
            const double bar = std::floor(t.beats / kBeatsPerBar), inBar = t.beats - bar * kBeatsPerBar;
            if (!haveLast_) start = inBar < kOnLineBeats || inBar > kBeatsPerBar - kOnLineBeats;
            else start = bar != std::floor(lastBeats_ / kBeatsPerBar) || t.beats < lastBeats_ - kWrapBeats;
            lastBeats_ = t.beats;
            haveLast_ = true;
        }
        if (start) {
            learner_.begin();
            learn_ = LearnState::Measuring;
            learnBeats_ = 0.0;
        }
        break;
    }
    case LearnState::Measuring:
        if (!t.playing) {
            learn_ = LearnState::Failed;
            learnError_ = "stopped";
        } else if (learnBeats_ >= kLearnBars * kBeatsPerBar - 1e-9) {
            const Measurement m = learner_.finish();
            if (m.valid) {
                measurement_ = m;
                learn_ = LearnState::Done;
            } else {
                learn_ = LearnState::Failed;
                learnError_ = "too quiet";
            }
        }
        break;
    default:
        break;
    }
}

void Master::process(float* L, float* R, int n) {
    if (learn_ == LearnState::Measuring) {   // the input, as the user plays it, and the beats it lasts
        learner_.process(L, R, n);
        learnBeats_ += n * bpm_ / 60.0 / ef::kRate;
    }

    tone_.process(L, R, n);
    if (glueOn_) glue_.process(L, R, n);
    if (gain_.moving() || gain_.value() != 1.0f) {
        for (int i = 0; i < n; ++i) {
            const float g = gain_.next();
            L[i] *= g;
            R[i] *= g;
        }
    }
    if (warmOn_) {
        warmth_.process(L, R, n);
    } else {
        for (int i = 0; i < n; ++i) {
            warmHist_[0][warmHistPos_] = L[i];
            warmHist_[1][warmHistPos_] = R[i];
            warmHistPos_ = (warmHistPos_ + 1) & (kWarmHistory - 1);
        }
    }
    width_.process(L, R, n);
    if (!clip_.idle()) {
        sanitize(L, n);   // as the clipper reads it
        sanitize(R, n);
        clipPeak_ = std::max(clipPeak_, std::max(peakOf(L, n), peakOf(R, n)));
        clip_.process(L, R, n);
    }
    limiter_.process(L, R, n);
    meter_.process(L, R, n);
}

void Master::learn() {
    learner_.begin();
    learn_ = LearnState::Armed;
    haveLast_ = false;
    learnBeats_ = 0.0;
    learnError_ = "";
}

void Master::cancelLearn() {
    if (learn_ == LearnState::Armed || learn_ == LearnState::Measuring) learn_ = LearnState::Idle;
}

float Master::takeReductionDb() {
    // The clipper's share: how far its input went over its level (it takes about that much off).
    const float clip = clipPeak_ > clipLevel_ ? 20.0f * std::log10(clipPeak_ / clipLevel_) : 0.0f;
    clipPeak_ = 0.0f;
    return clip + limiter_.takeReductionDb();
}

} // namespace mf
