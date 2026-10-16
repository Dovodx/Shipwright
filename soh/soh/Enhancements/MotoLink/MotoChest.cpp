// MotoLink: the key fob's chest in the Lost Woods, behind the stump the Skull Kid stands on (the Saria's Song
// Skull Kid, room 9).
//
// The chest is a normal En_Box spawned when the room loads, using a treasure flag nothing else in the Lost Woods
// uses, so it stays open once looted. Its vanilla contents are overridden the way the randomizer does it: Link
// plays the chest-opening animation without receiving anything, then the fob is handed over as a custom
// GetItemEntry (own 3D model and message) once he is free.
#include <libultraship/libultraship.h>

#include "MotoLink.h"
#include "soh/Enhancements/custom-message/CustomMessageManager.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Box/z_en_box.h"
extern PlayState* gPlayState;
void MotoLink_DrawGiKeyFob(PlayState* play, GetItemEntry* getItemEntry);
extern s32 Player_SetupWaitForPutAway(PlayState* play, Player* player, AfterPutAwayFunc func);
}
// Randomizer's "open chest, receive nothing yet" player action (randomizer/hook_handlers.cpp)
void func_8083A434_override(PlayState* play, Player* player);

namespace {

constexpr s16 kChestRoom = 9;
constexpr u8 kChestFlag = 0x1F; // unused by any Lost Woods actor
// Saria's Song Skull Kid's big stump, and the small stump Link plays from (room 9 actor list).
constexpr Vec3f kBigStump = { -1132.0f, 80.0f, -128.0f };
constexpr Vec3f kSmallStump = { -951.0f, 20.0f, 52.0f };
constexpr f32 kBehindStump = 170.0f; // past the big stump, seen from the small one (room for Link between)
constexpr u16 kTextKeyFob = 0x71A0; // custom message id (no vanilla or SoH message uses it)
constexpr s16 kGetItemKeyFob = 0x200; // outside the GI_* range so it never matches a vanilla item
constexpr u16 kModNone = 0;           // MOD_NONE

bool sGivePending = false;

GetItemEntry KeyFobEntry() {
    GetItemEntry entry = GET_ITEM(ITEM_NONE, OBJECT_GAMEPLAY_KEEP, GID_HEART_PIECE, kTextKeyFob, 0x80,
                                  CHEST_ANIM_LONG, ITEM_CATEGORY_MAJOR, kModNone, kGetItemKeyFob);
    entry.drawFunc = MotoLink_DrawGiKeyFob;
    return entry;
}

bool IsKeyFobChest(EnBox* chest) {
    return gPlayState->sceneNum == SCENE_LOST_WOODS && (chest->dyna.actor.params & 0x1F) == kChestFlag;
}

void SpawnChest() {
    PlayState* play = gPlayState;
    if (play == nullptr || play->sceneNum != SCENE_LOST_WOODS || play->roomCtx.curRoom.num != kChestRoom) {
        return;
    }
    for (Actor* a = play->actorCtx.actorLists[ACTORCAT_CHEST].head; a != nullptr; a = a->next) {
        if (a->id == ACTOR_EN_BOX && (a->params & 0x1F) == kChestFlag) {
            return; // room reloaded with the chest still present
        }
    }

    f32 dx = kBigStump.x - kSmallStump.x;
    f32 dz = kBigStump.z - kSmallStump.z;
    f32 len = sqrtf(dx * dx + dz * dz);
    Vec3f pos = { kBigStump.x + dx / len * kBehindStump, kBigStump.y + 200.0f, kBigStump.z + dz / len * kBehindStump };
    CollisionPoly* poly;
    s32 bgId;
    pos.y = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &pos);
    if (pos.y == BGCHECK_Y_MIN) {
        pos.y = kBigStump.y;
    }
    // En_Box opens from its local -Z side, so pointing +Z away from the stump puts the opening side towards the
    // gap between stump and chest, where Link arrives.
    s16 yaw = Math_Atan2S(dz, dx);
    // Big chest (type 0) holding a heart piece as far as the vanilla code knows; contents are overridden below.
    s16 params = (0 << 12) | (GI_HEART_PIECE << 5) | kChestFlag;
    Actor_Spawn(&play->actorCtx, play, ACTOR_EN_BOX, pos.x, pos.y, pos.z, 0, yaw, 0, params);
}

void BuildKeyFobMessage(uint16_t* textId, bool* loadFromMessageTable) {
    CustomMessage msg = CustomMessage(
        "You got the %rKey Fob%w! Use it from a %yC%w button to summon your motorcycle wherever Epona can go!",
        "Du hast den %rFunkschlüssel%w! Benutze ihn mit %yC%w, um dein Motorrad dorthin zu rufen, wo auch Epona "
        "hinkann!",
        "Vous obtenez la %rClé de moto%w! Utilisez-la avec %yC%w pour appeler votre moto partout où Epona peut aller!");
    // Plain auto-format (no textbox item icon: the icon path mis-renders for custom item IDs)
    msg.AutoFormat();
    msg.LoadIntoFont();
    *loadFromMessageTable = false;
}

void RegisterMotoChest() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneSpawnActors>(SpawnChest);

    REGISTER_VB_SHOULD(VB_GIVE_ITEM_FROM_CHEST, {
        EnBox* chest = va_arg(args, EnBox*);
        if (IsKeyFobChest(chest)) {
            Player_SetupWaitForPutAway(gPlayState, GET_PLAYER(gPlayState), func_8083A434_override);
            sGivePending = true;
            *should = false;
        }
    });

    // Hand the fob over once Link has finished opening the chest and is free.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayerUpdate>([]() {
        if (!sGivePending || gPlayState == nullptr) {
            return;
        }
        Player* player = GET_PLAYER(gPlayState);
        if (Player_InBlockingCsMode(gPlayState, player) || (player->stateFlags1 & PLAYER_STATE1_IN_ITEM_CS) ||
            (player->stateFlags1 & PLAYER_STATE1_GETTING_ITEM)) {
            return;
        }
        sGivePending = false;
        MotoLink_GiveKeyFob();
        GiveItemEntryWithoutActor(gPlayState, KeyFobEntry());
    });

    GameInteractor::Instance->RegisterGameHookForID<GameInteractor::OnOpenText>(kTextKeyFob, BuildKeyFobMessage);
}

static RegisterShipInitFunc initMotoChest(RegisterMotoChest);

} // namespace
