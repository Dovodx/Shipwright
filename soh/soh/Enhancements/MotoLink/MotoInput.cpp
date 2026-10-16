// MotoLink: analog trigger input for the motorcycle.
//
// The bike uses the gamepad triggers as analog throttle (whatever is mapped to N64 R, RT by default) and brake
// (whatever is mapped to N64 Z, LT by default). libultraship turns trigger axes into digital buttons, so the
// analog value is read back here from the SDL axis behind each R/Z mapping, which keeps user remaps working.
// Digital sources (keyboard, d-pad, N64 adapters) count as fully pressed.
#include <algorithm>
#include <cstdio>

#include <libultraship/libultraship.h>
#include <SDL2/SDL.h>

extern "C" {
#include "functions.h"
#include "variables.h"
// From z_en_horse_moto.c (its header uses `this` as a parameter name, so it can't be included from C++)
extern f32 gMotoThrottle;
extern f32 gMotoBrake;
extern u8 gMotoRiding;
void MotoLinkTest_OverrideInput(OSContPad* pad);
}

namespace {

// Largest analog value (0..1) among the SDL trigger/axis mappings bound to an N64 button on port 1, or -1 if no
// SDL axis is mapped to it (presses then come from digital sources only).
f32 ReadAxisBackedButton(CONTROLLERBUTTONS_T bitmask) {
    auto deck = Ship::Context::GetRawInstance()->GetControlDeck();
    auto controller = deck->GetControllerByPort(0);
    if (controller == nullptr) {
        return -1.0f;
    }
    f32 best = -1.0f;
    for (const auto& [id, mapping] : controller->GetButton(bitmask)->GetAllButtonMappings()) {
        int port, button, axis;
        char direction;
        // SDLAxisDirectionToButtonMapping ids look like "P0-B8192-SDLA5-ADP"
        if (sscanf(id.c_str(), "P%d-B%d-SDLA%d-AD%c", &port, &button, &axis, &direction) != 4) {
            continue;
        }
        for (const auto& [instanceId, gamepad] :
             deck->GetConnectedPhysicalDeviceManager()->GetConnectedSDLGamepadsForPort(port)) {
            f32 value = SDL_GameControllerGetAxis(gamepad, (SDL_GameControllerAxis)axis) / 32767.0f;
            best = std::max(best, std::clamp(direction == 'P' ? value : -value, 0.0f, 1.0f));
        }
    }
    return best;
}

f32 TriggerValue(CONTROLLERBUTTONS_T bitmask, bool digitallyPressed) {
    f32 analog = ReadAxisBackedButton(bitmask);
    if (analog > 0.02f) {
        return analog;
    }
    return digitallyPressed ? 1.0f : 0.0f;
}

} // namespace

// Called by PadMgr for controller 1 after its state is read, before press/release edges are computed.
extern "C" void MotoLink_FilterInput(OSContPad* pad) {
    MotoLinkTest_OverrideInput(pad);

    gMotoThrottle = TriggerValue(BTN_R, pad->button & BTN_R);
    gMotoBrake = TriggerValue(BTN_Z, pad->button & BTN_Z);

    // While riding, the triggers belong to the bike: no shield (R) or Z-targeting.
    if (gMotoRiding) {
        pad->button &= ~(BTN_R | BTN_Z);
    }
}
