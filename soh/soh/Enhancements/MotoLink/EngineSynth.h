#pragma once
// MotoLink: procedural motorcycle engine sound, rendered in real time.
//
// Header-only and dependency-free so the same code runs in the game's audio thread (MotoAudio.cpp) and in the
// offline renderer used to audition and compare it (MotoLink tools/engine_render.cpp).
//
// Voiced after recordings of Moto Racer's bike #2 (re/compare/engine_audio/moto_racer): a buzzy, tonal note whose
// harmonic series sits on a fundamental of ~25 Hz at idle rising to ~175 Hz at the redline (the tach's 12500 rpm),
// with the harmonic recipe changing with how hard the engine works:
//   - idle: a broad burble, harmonics 2..20 all strong (5th the strongest), soft;
//   - loaded / high revs: concentrated on the 2nd and 4th harmonics, falling off fast above the 6th.
// It is additive synthesis: the measured harmonic levels (in dB, relative to the strongest) are blended between
// the two recipes and summed. Grit comes from cycle-to-cycle roughness (each engine cycle's amplitude and pitch
// wobble slightly, as uneven combustion does) and a little noise under load.
#include <cmath>
#include <cstdint>

namespace MotoLink {

class EngineSynth {
  public:
    explicit EngineSynth(float sampleRate) : mSampleRate(sampleRate) {
        for (int i = 0; i < kTableSize; i++) {
            mSine[i] = std::sin(2.0f * 3.14159265f * i / kTableSize);
        }
    }

    // Target parameters; the synth glides towards them so per-frame updates never click.
    // rpm: engine speed (tach rpm). load: throttle 0..1. pitch: extra frequency factor (Doppler), 1 = none.
    void SetTarget(float rpm, float load, float pitch = 1.0f) {
        mTargetRpm = rpm;
        mTargetLoad = load;
        mTargetPitch = pitch;
    }

    // Underwater: low-pass the output (faded in/out so it never clicks).
    void SetMuffled(bool muffled) {
        mTargetMuffle = muffled ? 1.0f : 0.0f;
    }

    // Tyre screech level 0..1 (hard braking at speed), faded towards.
    void SetSkid(float level) {
        mTargetSkid = level;
    }

    // A landing/impact thump of the given strength 0..1 (suspension bottoming out, tyres hitting the ground).
    void Thump(float strength) {
        mThumpEnv = std::fmax(mThumpEnv, strength);
        mThumpPhase = 0.0f;
    }

    // Renders `frames` mono samples (roughly -1..1) into `out`.
    void Render(float* out, uint32_t frames) {
        const float glide = 1.0f - std::exp(-1.0f / (0.012f * mSampleRate)); // ~12 ms time constant
        for (uint32_t i = 0; i < frames; i++) {
            mRpm += (mTargetRpm - mRpm) * glide;
            mLoad += (mTargetLoad - mLoad) * glide;
            mPitch += (mTargetPitch - mPitch) * glide;
            mMuffle += (mTargetMuffle - mMuffle) * glide;

            if ((i & 31) == 0) {
                UpdateHarmonics(); // the recipe changes slowly: every 32 samples is plenty
            }

            // One engine cycle per period of the fundamental; each gets its own small amplitude/pitch wobble.
            const float f0 = Fundamental(mRpm) * mPitch * (1.0f + mCycleJitter);
            mPhase += f0 / mSampleRate;
            if (mPhase >= 1.0f) {
                mPhase -= 1.0f;
                mCycleGain = 1.0f + kRoughness * (Random() * 2.0f - 1.0f);
                mCycleJitter = kPitchJitter * (Random() * 2.0f - 1.0f);
            }
            mGain += (mCycleGain - mGain) * 0.02f; // smoothed so it roughens rather than clicks

            float y = 0.0f;
            const uint32_t base = (uint32_t)(mPhase * kTableSize);
            for (int k = 0; k < mNumHarmonics; k++) {
                y += mAmp[k] * mSine[(base * (k + 1)) & (kTableSize - 1)];
            }
            y *= mGain * mLevel;

            // A little breath under load (intake/exhaust noise), pulsing with the engine cycle.
            const float noise = (Random() * 2.0f - 1.0f) * mLoad * 0.05f * (0.5f + 0.5f * mSine[base & (kTableSize - 1)]);
            mNoiseLp += (noise - mNoiseLp) * 0.35f;
            y += mNoiseLp;

            // Tyre screech: noise through two resonances (~1.9 and ~3.1 kHz) with a slow wobble, faded in and out.
            mSkid += (mTargetSkid - mSkid) * (glide * 0.25f);
            if (mSkid > 1e-4f) {
                const float n = Random() * 2.0f - 1.0f;
                mSkidWobble += 2.0f * 3.14159265f * 7.0f / mSampleRate;
                const float wob = 1.0f + 0.04f * std::sin(mSkidWobble);
                y += 1.6f * mSkid *
                     (mSkidA.Process(n, 1900.0f * wob, 0.995f, mSampleRate) + 0.6f * mSkidB.Process(n, 3100.0f * wob, 0.993f, mSampleRate));
            }
            // Thump: a short, falling low tone with a burst of noise.
            if (mThumpEnv > 1e-4f) {
                mThumpPhase += (55.0f + 60.0f * mThumpEnv) / mSampleRate;
                y += mThumpEnv * (0.9f * std::sin(2.0f * 3.14159265f * mThumpPhase) + 0.25f * (Random() * 2.0f - 1.0f));
                mThumpEnv *= 0.99955f; // ~70 ms
            }

            // Gentle saturation keeps peaks round when everything lines up.
            y = std::tanh(1.3f * y) / 1.3f;

            // ~400 Hz one-pole low-pass, cross-faded in when muffled
            mMuffled += (y - mMuffled) * (1.0f - std::exp(-2.0f * 3.14159265f * 400.0f / mSampleRate));
            out[i] = y + (1.6f * mMuffled - y) * mMuffle;
        }
    }

    // Fundamental (harmonic spacing) for a tach rpm: Moto Racer's ~25 Hz at idle to ~175 Hz at the redline.
    static float Fundamental(float rpm) {
        return kIdleHz + (rpm - kIdleRpm) * ((kRedlineHz - kIdleHz) / (kRedlineRpm - kIdleRpm));
    }

  private:
    static constexpr int kTableSize = 4096;
    static constexpr int kMaxHarmonics = 24;
    static constexpr float kIdleRpm = 800.0f, kRedlineRpm = 12500.0f;
    static constexpr float kIdleHz = 25.2f, kRedlineHz = 175.0f;
    static constexpr float kRoughness = 0.18f;  // per-cycle amplitude wobble
    static constexpr float kPitchJitter = 0.006f;
    static constexpr float kMaxFreq = 7000.0f;  // Moto Racer's samples have nothing above ~7 kHz

    // Harmonic levels in dB (harmonics 1..24) measured from Moto Racer: idle and at the rev limiter.
    static constexpr float kIdleDb[kMaxHarmonics] = { -31, -9,  -13, -13, 0,   -4,  -4,  -8,  -8,  -6,  -9,  -6,
                                                      -8,  -9,  -11, -10, -9,  -11, -12, -13, -11, -10, -10, -15 };
    static constexpr float kLoadDb[kMaxHarmonics] = { -17, -4,  -29, 0,   -14, -15, -25, -28, -30, -32, -33, -29,
                                                      -41, -41, -43, -39, -43, -39, -39, -42, -43, -43, -43, -46 };

    // Blends the two recipes by how hard the engine works (revs and throttle) and sets the output level: idle sits
    // ~6.5 dB under full load, as in the recordings.
    void UpdateHarmonics() {
        const float revs = std::fmin(std::fmax((mRpm - kIdleRpm) / (kRedlineRpm - kIdleRpm), 0.0f), 1.0f);
        const float work = std::fmin(1.0f, 0.75f * std::sqrt(revs) + 0.45f * mLoad);
        const float f0 = Fundamental(mRpm) * mPitch;
        float sum = 0.0f;
        mNumHarmonics = 0;
        for (int k = 0; k < kMaxHarmonics; k++) {
            if (f0 * (k + 1) > kMaxFreq) {
                break;
            }
            const float db = kIdleDb[k] + (kLoadDb[k] - kIdleDb[k]) * work;
            mAmp[k] = std::pow(10.0f, db / 20.0f);
            sum += mAmp[k] * mAmp[k];
            mNumHarmonics = k + 1;
        }
        // Normalise to a constant RMS for the recipe, then apply the level for this much work.
        const float norm = sum > 0.0f ? 1.0f / std::sqrt(sum) : 0.0f;
        const float level = 0.47f + 0.53f * work; // -6.5 dB at idle
        for (int k = 0; k < mNumHarmonics; k++) {
            mAmp[k] *= norm;
        }
        mLevel = 0.55f * level;
    }

    float Random() {
        mSeed = mSeed * 1664525u + 1013904223u;
        return (mSeed >> 8) * (1.0f / 16777216.0f);
    }

    float mSampleRate;
    float mSine[kTableSize];
    float mAmp[kMaxHarmonics] = {};
    int mNumHarmonics = 0;
    float mLevel = 0.0f;
    float mTargetRpm = 800.0f, mTargetLoad = 0.0f, mTargetPitch = 1.0f;
    float mRpm = 800.0f, mLoad = 0.0f, mPitch = 1.0f;
    float mPhase = 0.0f, mGain = 1.0f, mCycleGain = 1.0f, mCycleJitter = 0.0f;
    float mNoiseLp = 0.0f;
    float mTargetSkid = 0.0f, mSkid = 0.0f, mSkidWobble = 0.0f;
    float mThumpEnv = 0.0f, mThumpPhase = 0.0f;
    struct Resonator {
        float y1 = 0.0f, y2 = 0.0f;
        float Process(float x, float freq, float r, float sampleRate) {
            const float w = 2.0f * 3.14159265f * freq / sampleRate;
            const float y = 2.0f * r * std::cos(w) * y1 - r * r * y2 + x * (1.0f - r);
            y2 = y1;
            y1 = y;
            return y;
        }
    };
    Resonator mSkidA, mSkidB;
    float mTargetMuffle = 0.0f, mMuffle = 0.0f, mMuffled = 0.0f;
    uint32_t mSeed = 22222u;
};

} // namespace MotoLink
