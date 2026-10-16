// MotoLink: Moto Racer motorcycle that rides alongside Epona (see DESIGN_GAPS.md in the MotoLink project).
// This file holds the C++ side: console commands and, later, CVars/hooks. Bike behaviour lives in
// src/overlays/actors/ovl_En_Horse/z_en_horse_moto.c.

#include <libultraship/libultraship.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "functions.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Horse/z_en_horse.h"
extern PlayState* gPlayState;
// From z_en_horse_moto.c (its header uses `this` as a parameter name, so it can't be included from C++)
extern u8 gMotoRiding;
void EnHorseMoto_DrawTach(PlayState* play);
}

namespace {

constexpr f32 kSpawnSideOffset = 60.0f; // units to Link's right, clear of his collision cylinder

// moto: spawn a riderless HORSE_MOTO beside Link, facing the same way.
bool MotoSpawnHandler(std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                      std::string* output) {
    if (gPlayState == nullptr) {
        *output += "No PlayState (load a save first)";
        return 1;
    }

    Player* player = GET_PLAYER(gPlayState);
    s16 yaw = player->actor.shape.rot.y;
    // Right of Link = forward rotated -90 degrees.
    f32 x = player->actor.world.pos.x - Math_CosS(yaw) * kSpawnSideOffset;
    f32 z = player->actor.world.pos.z + Math_SinS(yaw) * kSpawnSideOffset;

    Actor* bike = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_HORSE, x, player->actor.world.pos.y, z, 0,
                              yaw, 0, ENHORSE_PARAM_MOTO);
    if (bike == nullptr) {
        *output += "Failed to spawn the bike";
        return 1;
    }
    *output += "Spawned bike";
    return 0;
}

void RegisterMotoLink() {
    Ship::Context::GetRawInstance()->GetConsole()->AddCommand(
        "moto", { MotoSpawnHandler, "MotoLink: spawn the motorcycle beside Link.", {} });

    // The tachometer replaces Epona's carrots while riding the bike.
    REGISTER_VB_SHOULD(VB_DRAW_EPONA_BOOST_CARROTS, {
        if (gMotoRiding) {
            *should = false;
        }
    });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDrawEnd>([]() {
        if (gPlayState != nullptr) {
            EnHorseMoto_DrawTach(gPlayState);
        }
    });
}

static RegisterShipInitFunc initMotoLink(RegisterMotoLink);

} // namespace
