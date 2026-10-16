// MotoLink: the key fob item (ITEM_KEY_FOB), used from a C button to summon the motorcycle.
//
// The 24 inventory slots are all taken in vanilla, so while Link is adult the fob lives in the child trade slot
// (whose items are child-only and unusable as an adult anyway): the child trade item is stashed in the MotoLink
// save section and put back whenever Link is a child.
#include <cstring>

#include <libultraship/libultraship.h>

#include "MotoLink.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/SaveManager.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
#include "soh_assets.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
extern PlayState* gPlayState;
}

#define CVAR_START_WITH_KEY_FOB CVAR_ENHANCEMENT("MotoLink.StartWithKeyFob")

namespace {

// Puts the fob into (adult) or takes it out of (child) the child trade slot, keeping the child item safe.
void SyncFobSlot() {
    u8& slot = gSaveContext.inventory.items[SLOT_TRADE_CHILD];
    if (gMotoSave.hasKeyFob && LINK_IS_ADULT) {
        if (slot != ITEM_KEY_FOB) {
            gMotoSave.stashedChildTradeItem = slot;
            slot = ITEM_KEY_FOB;
        }
    } else if (slot == ITEM_KEY_FOB) {
        slot = gMotoSave.stashedChildTradeItem;
        gMotoSave.stashedChildTradeItem = ITEM_NONE;
    }
}

void UseKeyFob() {
    PlayState* play = gPlayState;
    // Same places Epona can be called (Hyrule Field, Lake Hylia, Gerudo Valley/Fortress, Lon Lon Ranch).
    if (play == nullptr || !Horse_CanSpawn(play->sceneNum) || !MotoLink_SummonBike(play)) {
        Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        return;
    }
    Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
}

bool KeyFobCommand(std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                   std::string* output) {
    MotoLink_GiveKeyFob();
    *output += "Got the Key Fob";
    return 0;
}

void RegisterMotoFob() {
    // Using the fob from a C button summons the bike instead of making Link hold an item.
    REGISTER_VB_SHOULD(VB_CHANGE_HELD_ITEM_AND_USE_ITEM, {
        int32_t item = va_arg(args, int32_t);
        if (item == ITEM_KEY_FOB) {
            *should = false;
            UseKeyFob();
        }
    });
    // The fob has no player item action (sItemActions stops long before custom items).
    REGISTER_VB_SHOULD(VB_ITEM_ACTION_BE_NONE, {
        int32_t item = va_arg(args, int32_t);
        if (item == ITEM_KEY_FOB) {
            *should = true;
        }
    });
    // The child trade slot is child-only; while it holds the fob it belongs to adult Link.
    REGISTER_VB_SHOULD(VB_SLOT_MEETS_AGE_REQ, {
        int32_t slot = va_arg(args, int32_t);
        if (slot == SLOT_TRADE_CHILD && gSaveContext.inventory.items[SLOT_TRADE_CHILD] == ITEM_KEY_FOB) {
            *should = LINK_IS_ADULT;
        }
    });
    REGISTER_VB_SHOULD(VB_DRAW_CUSTOM_ITEM_NAME, {
        u32 namedItem = va_arg(args, u32);
        if (namedItem == ITEM_KEY_FOB) {
            *should = true;
            const char* texture = gKeyFobItemNameENGTex;
            if (gSaveContext.language == LANGUAGE_GER) {
                texture = gKeyFobItemNameGERTex;
            } else if (gSaveContext.language == LANGUAGE_FRA) {
                texture = gKeyFobItemNameFRATex;
            }
            memcpy(gPlayState->pauseCtx.nameSegment, texture, strlen(texture) + 1);
        }
    });

    // Age changes always come with a scene load.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneInit>([](int16_t sceneNum) { SyncFobSlot(); });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnLoadGame>([](int32_t fileNum) {
        if (CVarGetInteger(CVAR_START_WITH_KEY_FOB, 0)) {
            gMotoSave.hasKeyFob = true;
        }
        SyncFobSlot();
    });

    Ship::Context::GetRawInstance()->GetConsole()->AddCommand(
        "keyfob", { KeyFobCommand, "MotoLink: give Link the key fob.", {} });
}

static RegisterShipInitFunc initMotoFob(RegisterMotoFob);

} // namespace

extern "C" void MotoLink_GiveKeyFob(void) {
    gMotoSave.hasKeyFob = true;
    SyncFobSlot();
}
