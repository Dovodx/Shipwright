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

EnHorse* FindHorse(PlayState* play, s16 type) {
    for (Actor* actor = play->actorCtx.actorLists[ACTORCAT_BG].head; actor != nullptr; actor = actor->next) {
        if (actor->id == ACTOR_EN_HORSE && actor->update != nullptr && ((EnHorse*)actor)->type == type) {
            return (EnHorse*)actor;
        }
    }
    return nullptr;
}

} // namespace

// Brings the bike to Link's side: moves the bike already in the scene, or spawns one. Only one mount is out at a
// time, so an unridden Epona is sent away. Returns false if Link is riding something already.
extern "C" bool MotoLink_SummonBike(PlayState* play) {
    Player* player = GET_PLAYER(play);
    if (player->rideActor != nullptr) {
        return false;
    }

    EnHorse* epona = FindHorse(play, HORSE_EPONA);
    if (epona != nullptr) {
        Actor_Kill(&epona->actor);
    }

    s16 yaw = player->actor.shape.rot.y;
    // Right of Link = forward rotated -90 degrees.
    f32 x = player->actor.world.pos.x - Math_CosS(yaw) * kSpawnSideOffset;
    f32 z = player->actor.world.pos.z + Math_SinS(yaw) * kSpawnSideOffset;
    f32 y = player->actor.world.pos.y;

    EnHorse* bike = FindHorse(play, HORSE_MOTO);
    if (bike != nullptr) {
        bike->actor.world.pos = bike->actor.home.pos = { x, y, z };
        bike->actor.prevPos = bike->actor.world.pos;
        bike->actor.world.rot.y = bike->actor.shape.rot.y = yaw;
        bike->actor.speedXZ = 0.0f;
        return true;
    }
    return Actor_Spawn(&play->actorCtx, play, ACTOR_EN_HORSE, x, y, z, 0, yaw, 0, ENHORSE_PARAM_MOTO) != nullptr;
}

namespace {

// moto: summon the bike beside Link.
bool MotoSpawnHandler(std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                      std::string* output) {
    if (gPlayState == nullptr) {
        *output += "No PlayState (load a save first)";
        return 1;
    }
    if (!MotoLink_SummonBike(gPlayState)) {
        *output += "Can't summon while riding";
        return 1;
    }
    *output += "Summoned bike";
    return 0;
}

void RegisterMotoLink() {
    Ship::Context::GetRawInstance()->GetConsole()->AddCommand(
        "moto", { MotoSpawnHandler, "MotoLink: summon the motorcycle beside Link.", {} });

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
