// MotoLink: engine audio. Each bike owns a voice of the procedural EngineSynth; the game thread publishes its
// RPM, throttle load, stereo gains and Doppler factor every frame, and the audio thread renders all active voices
// into SoH's output buffer just before it is handed to the audio player (OTRAudio_Thread in OTRGlobals.cpp).
//
// Mixing outside the N64 audio engine means the always-running engines never take sequence/SFX channels away
// from the game's own sounds, and pitch/Doppler are continuous.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>

#include <libultraship/bridge/consolevariablebridge.h>

#include <libultraship/libultra/types.h>

#include "EngineSynth.h"
#include "soh/cvar_prefixes.h"

extern "C" {
float AudioSettings_GetMasterVolume(void);
}

namespace {

constexpr int kMaxVoices = 4;
constexpr float kSampleRate = 32000.0f;
// Engine level in the game's mix (gMotoLink.Audio.Volume scales it further).
constexpr float kEngineLevel = 0.5f;

struct VoiceParams {
    bool active = false;
    float rpm = 800.0f;
    float pitch = 1.0f;
    float load = 0.0f;
    float skid = 0.0f;
    uint32_t thumps = 0;      // count of thumps requested (a change triggers one)
    float thumpStrength = 0.0f;
    float gainL = 0.0f, gainR = 0.0f;
    bool muffled = false;
};

struct Voice {
    MotoLink::EngineSynth synth{ kSampleRate };
    uint32_t thumps = 0;
    float gainL = 0.0f, gainR = 0.0f; // smoothed per buffer
};

std::mutex sParamsMutex;
std::array<VoiceParams, kMaxVoices> sParams; // written by the game thread
std::array<Voice, kMaxVoices> sVoices;       // owned by the audio thread

} // namespace

extern "C" {

// Returns a voice index, or -1 when all voices are taken (the extra bike is then silent).
s32 MotoAudio_AllocVoice(void) {
    std::lock_guard<std::mutex> lock(sParamsMutex);
    for (int i = 0; i < kMaxVoices; i++) {
        if (!sParams[i].active) {
            sParams[i] = VoiceParams{};
            sParams[i].active = true;
            return i;
        }
    }
    return -1;
}

void MotoAudio_FreeVoice(s32 voice) {
    if (voice < 0 || voice >= kMaxVoices) {
        return;
    }
    std::lock_guard<std::mutex> lock(sParamsMutex);
    sParams[voice].active = false;
}

// pitch is the Doppler factor (1 = none); gains are linear 0..1 per channel; muffled = underwater.
void MotoAudio_SetVoice(s32 voice, f32 rpm, f32 pitch, f32 load, f32 gainL, f32 gainR, u8 muffled) {
    if (voice < 0 || voice >= kMaxVoices) {
        return;
    }
    std::lock_guard<std::mutex> lock(sParamsMutex);
    VoiceParams& p = sParams[voice];
    p.rpm = rpm;
    p.pitch = pitch;
    p.load = load;
    p.gainL = gainL;
    p.gainR = gainR;
    p.muffled = muffled;
}

// Adds every active engine voice to an interleaved stereo s16 buffer. Audio thread only.
void MotoAudio_Mix(s16* buffer, u32 frames) {
    std::array<VoiceParams, kMaxVoices> params;
    {
        std::lock_guard<std::mutex> lock(sParamsMutex);
        params = sParams;
    }
    const float volume = AudioSettings_GetMasterVolume() *
                         (CVarGetInteger(CVAR_SETTING("Volume.SFX"), 100) / 100.0f) * kEngineLevel *
                         CVarGetFloat("gMotoLink.Audio.Volume", 1.0f);
    static float mono[2048];

    for (int v = 0; v < kMaxVoices; v++) {
        Voice& voice = sVoices[v];
        const VoiceParams& p = params[v];
        const float targetL = p.active ? p.gainL * volume : 0.0f;
        const float targetR = p.active ? p.gainR * volume : 0.0f;
        if (!p.active && voice.gainL < 1e-4f && voice.gainR < 1e-4f) {
            continue;
        }
        voice.synth.SetTarget(p.rpm, p.load, p.pitch);
        voice.synth.SetSkid(p.skid);
        if (voice.thumps != p.thumps) {
            voice.thumps = p.thumps;
            voice.synth.Thump(p.thumpStrength);
        }
        voice.synth.SetMuffled(p.muffled);
        for (u32 start = 0; start < frames; start += 2048) {
            const u32 n = std::min<u32>(2048, frames - start);
            voice.synth.Render(mono, n);
            // Ramp gains across the block so volume/pan changes and voice start/stop never click.
            const float startL = voice.gainL, startR = voice.gainR;
            for (u32 i = 0; i < n; i++) {
                const float t = (i + 1) / (float)n;
                const float l = startL + (targetL - startL) * t;
                const float r = startR + (targetR - startR) * t;
                s16* out = &buffer[(start + i) * 2];
                out[0] = (s16)std::clamp(out[0] + mono[i] * l * 32767.0f, -32768.0f, 32767.0f);
                out[1] = (s16)std::clamp(out[1] + mono[i] * r * 32767.0f, -32768.0f, 32767.0f);
            }
            voice.gainL = targetL;
            voice.gainR = targetR;
        }
    }
}

// Tyre screech level 0..1 for a voice.
void MotoAudio_SetSkid(s32 voice, f32 level) {
    if (voice < 0 || voice >= kMaxVoices) {
        return;
    }
    std::lock_guard<std::mutex> lock(sParamsMutex);
    sParams[voice].skid = level;
}

// A landing/impact thump of strength 0..1 for a voice.
void MotoAudio_Thump(s32 voice, f32 strength) {
    if (voice < 0 || voice >= kMaxVoices) {
        return;
    }
    std::lock_guard<std::mutex> lock(sParamsMutex);
    sParams[voice].thumps++;
    sParams[voice].thumpStrength = strength;
}

} // extern "C"
