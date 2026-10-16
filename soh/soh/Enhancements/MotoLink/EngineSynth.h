#pragma once
// MotoLink: procedural motorcycle engine sound (inline-four sport bike), rendered in real time.
//
// Header-only and dependency-free so the same code runs in the game's audio thread (MotoAudio.cpp) and in the
// offline renderer used to audition and compare it (MotoLink tools/engine_render.cpp).
//
// Model: a 4-stroke inline-four fires twice per crank revolution, so pulses arrive at rpm/30 Hz (27 Hz at an
// 800 rpm idle, 417 Hz at a 12500 rpm redline). Each pulse excites two resonators - a low "exhaust body" and a
// higher "bark" whose level rises with throttle load - and a little firing-synchronous noise; a low-passed
// sawtooth at the firing frequency adds the steady tone that carries the pitch.
#include <cmath>
#include <cstdint>

namespace MotoLink {

class EngineSynth {
  public:
    explicit EngineSynth(float sampleRate) : mSampleRate(sampleRate) {
    }

    // Target parameters; the synth glides towards them so per-frame updates never click.
    // rpm: engine speed (already multiplied by any Doppler factor). load: throttle 0..1.
    void SetTarget(float rpm, float load) {
        mTargetRpm = rpm;
        mTargetLoad = load;
    }

    // Renders `frames` mono samples (roughly -1..1) into `out`.
    void Render(float* out, uint32_t frames) {
        const float glide = 1.0f - std::exp(-1.0f / (0.012f * mSampleRate)); // ~12 ms time constant
        for (uint32_t i = 0; i < frames; i++) {
            mRpm += (mTargetRpm - mRpm) * glide;
            mLoad += (mTargetLoad - mLoad) * glide;

            const float fire = mRpm / 30.0f;
            mPhase += fire / mSampleRate;
            float excite = 0.0f;
            if (mPhase >= 1.0f) {
                mPhase -= 1.0f;
                // Cylinder-to-cylinder variation keeps it from sounding like a pure buzz.
                excite = (0.75f + 0.5f * Random()) * (0.45f + 0.55f * mLoad);
                mNoiseEnv = 1.0f;
            }

            const float body = mBody.Process(excite, 85.0f + 0.032f * mRpm, 0.92f, mSampleRate);
            const float bark = mBark.Process(excite, 650.0f + 0.07f * mRpm, 0.965f, mSampleRate);

            const float saw = 2.0f * mPhase - 1.0f;
            mTone += (saw - mTone) * (1.0f - std::exp(-2.0f * 3.14159265f * (2.5f * fire) / mSampleRate));

            mNoiseEnv *= 0.9985f;
            const float noise = (Random() * 2.0f - 1.0f) * mNoiseEnv * (0.05f + 0.12f * mLoad);

            const float y = 2.2f * body + (0.5f + 1.1f * mLoad) * bark + 0.35f * mTone + noise;
            out[i] = std::tanh(y * 0.9f);
        }
    }

  private:
    // Two-pole resonator (decaying sinusoid per impulse); `r` sets the ring time.
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

    float Random() {
        mSeed = mSeed * 1664525u + 1013904223u;
        return (mSeed >> 8) * (1.0f / 16777216.0f);
    }

    float mSampleRate;
    float mTargetRpm = 800.0f, mTargetLoad = 0.0f;
    float mRpm = 800.0f, mLoad = 0.0f;
    float mPhase = 0.0f, mTone = 0.0f, mNoiseEnv = 0.0f;
    Resonator mBody, mBark;
    uint32_t mSeed = 22222u;
};

} // namespace MotoLink
