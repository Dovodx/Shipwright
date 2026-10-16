// MotoLink: per-save bike state and the bike's presence in scenes.
//
// The bike is a single persistent object: it stays where it was last left (initially parked at Lon Lon Ranch),
// rides through scene exits with Link, and is sent "away" when Epona's song calls Epona (the key fob brings it
// back). Its state lives in the "motolink" SaveManager section so vanilla save data is untouched.
#include <libultraship/libultraship.h>

#include "MotoLink.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/SaveManager.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Horse/z_en_horse.h"
extern PlayState* gPlayState;
}

MotoLinkSave gMotoSave;
static bool sRacingOnBike = false; // per race, not saved (like the race state in eventInf)

namespace {

// Where the bike is parked on a new file: Lon Lon Ranch, one of the ranch's always-loaded horse spawn points.
constexpr Vec3f kRanchParkingPos = { 856.0f, 0.0f, -918.0f };
constexpr s16 kRanchParkingYaw = 0;

bool sSectionLoaded = false;

void ResetToDefaults() {
    gMotoSave = {};
    gMotoSave.stashedChildTradeItem = ITEM_NONE;
    gMotoSave.bikeScene = SCENE_LON_LON_RANCH;
    gMotoSave.bikePos = kRanchParkingPos;
    gMotoSave.bikeYaw = kRanchParkingYaw;
}

void SaveSection(const SaveContext& saveContext, int sectionID, bool fullSave) {
    SaveManager::Instance->SaveData("hasKeyFob", gMotoSave.hasKeyFob);
    SaveManager::Instance->SaveData("ridingBike", gMotoSave.ridingBike);
    SaveManager::Instance->SaveData("stashedChildTradeItem", gMotoSave.stashedChildTradeItem);
    SaveManager::Instance->SaveData("bikeScene", gMotoSave.bikeScene);
    SaveManager::Instance->SaveStruct("bikePos", []() {
        SaveManager::Instance->SaveData("x", gMotoSave.bikePos.x);
        SaveManager::Instance->SaveData("y", gMotoSave.bikePos.y);
        SaveManager::Instance->SaveData("z", gMotoSave.bikePos.z);
    });
    SaveManager::Instance->SaveData("bikeYaw", gMotoSave.bikeYaw);
}

void LoadSection() {
    ResetToDefaults();
    SaveManager::Instance->LoadData("hasKeyFob", gMotoSave.hasKeyFob);
    SaveManager::Instance->LoadData("ridingBike", gMotoSave.ridingBike);
    SaveManager::Instance->LoadData("stashedChildTradeItem", gMotoSave.stashedChildTradeItem, (u8)ITEM_NONE);
    SaveManager::Instance->LoadData("bikeScene", gMotoSave.bikeScene, (s16)SCENE_LON_LON_RANCH);
    SaveManager::Instance->LoadStruct("bikePos", []() {
        SaveManager::Instance->LoadData("x", gMotoSave.bikePos.x, kRanchParkingPos.x);
        SaveManager::Instance->LoadData("y", gMotoSave.bikePos.y, kRanchParkingPos.y);
        SaveManager::Instance->LoadData("z", gMotoSave.bikePos.z, kRanchParkingPos.z);
    });
    SaveManager::Instance->LoadData("bikeYaw", gMotoSave.bikeYaw, kRanchParkingYaw);
    sSectionLoaded = true;
}

EnHorse* FindBike(PlayState* play) {
    for (Actor* actor = play->actorCtx.actorLists[ACTORCAT_BG].head; actor != nullptr; actor = actor->next) {
        if (actor->id == ACTOR_EN_HORSE && actor->update != nullptr && ((EnHorse*)actor)->type == HORSE_MOTO) {
            return (EnHorse*)actor;
        }
    }
    return nullptr;
}

// Re-creates the parked bike when entering its scene. The bike is adult-only, like Epona.
void OnSceneSpawnActors() {
    PlayState* play = gPlayState;
    if (play == nullptr || !LINK_IS_ADULT || gMotoSave.bikeScene != play->sceneNum || FindBike(play) != nullptr) {
        return;
    }
    Actor* bike = Actor_Spawn(&play->actorCtx, play, ACTOR_EN_HORSE, gMotoSave.bikePos.x, gMotoSave.bikePos.y,
                              gMotoSave.bikePos.z, 0, gMotoSave.bikeYaw, 0, ENHORSE_PARAM_MOTO);
    if (bike != nullptr && play->sceneNum == SCENE_GERUDOS_FORTRESS) {
        bike->room = -1; // like Epona: the fortress spans rooms
    }
}

// Epona's song brings Epona, and only one mount is out at a time: an unridden bike goes away.
void OnOcarinaSongAction() {
    PlayState* play = gPlayState;
    if (play == nullptr || play->msgCtx.lastPlayedSong != OCARINA_SONG_EPONAS || !Horse_CanSpawn(play->sceneNum)) {
        return;
    }
    EnHorse* bike = FindBike(play);
    if (bike != nullptr && !bike->playerControlled) {
        Actor_Kill(&bike->actor);
        gMotoSave.bikeScene = -1;
    }
}

void RegisterMotoSave() {
    ResetToDefaults();
    SaveManager::Instance->AddInitFunction([](bool isDebug) { ResetToDefaults(); });
    SaveManager::Instance->AddLoadFunction("motolink", 1, LoadSection);
    SaveManager::Instance->AddSaveFunction("motolink", 1, SaveSection, true, SECTION_PARENT_NONE);
    // Saves without a motolink section (older files, the debug save) start from the defaults.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnLoadGame>([](int32_t fileNum) {
        if (!sSectionLoaded) {
            ResetToDefaults();
        }
        sSectionLoaded = false;
    });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneSpawnActors>(OnSceneSpawnActors);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneInit>([](int16_t sceneNum) {
        if (sceneNum != SCENE_LON_LON_RANCH) {
            sRacingOnBike = false; // the race (and its escape) is over once Link leaves the ranch
        }
    });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnOcarinaSongAction>(OnOcarinaSongAction);
}

static RegisterShipInitFunc initMotoSave(RegisterMotoSave);

} // namespace

extern "C" void MotoLink_TrackBike(PlayState* play, Actor* bike, bool ridden) {
    gMotoSave.ridingBike = ridden;
    gMotoSave.bikeScene = play->sceneNum;
    gMotoSave.bikePos = bike->world.pos;
    gMotoSave.bikeYaw = bike->shape.rot.y;
}

extern "C" bool MotoLink_PlayerOnBike(PlayState* play) {
    Player* player = GET_PLAYER(play);
    return (player->stateFlags1 & PLAYER_STATE1_ON_HORSE) && player->rideActor != nullptr &&
           player->rideActor->id == ACTOR_EN_HORSE && ((EnHorse*)player->rideActor)->type == HORSE_MOTO;
}

extern "C" s32 MotoLink_MountParamsFlag(void) {
    return (gMotoSave.ridingBike || sRacingOnBike) ? ENHORSE_PARAM_MOTO : 0;
}

extern "C" void MotoLink_SetRacingOnBike(bool racingOnBike) {
    sRacingOnBike = racingOnBike;
}

extern "C" bool MotoLink_SetupArrivingOnBike(PlayState* play, Player* player) {
    if (AREG(6) == 0 || !gMotoSave.ridingBike) {
        return false;
    }
    // Params 9 is the "arriving mounted" setup Epona uses for the same case.
    player->rideActor =
        Actor_Spawn(&play->actorCtx, play, ACTOR_EN_HORSE, player->actor.world.pos.x, player->actor.world.pos.y,
                    player->actor.world.pos.z, player->actor.shape.rot.x, player->actor.shape.rot.y,
                    player->actor.shape.rot.z, 9 | ENHORSE_PARAM_MOTO);
    if (player->rideActor == nullptr) {
        return false;
    }
    Actor_MountHorse(play, player, player->rideActor);
    Actor_RequestHorseCameraSetting(play, player);
    if (play->sceneNum == SCENE_GERUDOS_FORTRESS) {
        player->rideActor->room = -1;
    }
    return true;
}
