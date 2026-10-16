#ifndef MOTOLINK_H
#define MOTOLINK_H

// MotoLink: state and entry points shared between the decomp (C) and the enhancement (C++).
#include <libultraship/libultra/types.h>
#include "z64math.h"

#ifdef __cplusplus
extern "C" {
#endif

struct PlayState;
struct Player;
struct Actor;

// Per-save state, persisted in the "motolink" SaveManager section (MotoSave.cpp).
typedef struct {
    u8 hasKeyFob;
    u8 ridingBike;           // the player was on the bike (survives the scene change when riding through an exit)
    u8 stashedChildTradeItem; // child trade item held aside while the fob occupies SLOT_TRADE_CHILD as adult
    s16 bikeScene;           // scene the bike is parked in, or -1 when it is "away" (summon it with the fob)
    Vec3f bikePos;
    s16 bikeYaw;
} MotoLinkSave;

extern MotoLinkSave gMotoSave;

// Brings the bike to Link's side (moving it if it is in the scene). False if Link is already riding something.
bool MotoLink_SummonBike(struct PlayState* play);
// Called by Horse_SetupInGameplay when Link arrives in a scene riding (AREG(6)). Returns true if it spawned the
// bike under him, in which case Epona's setup is skipped.
bool MotoLink_SetupArrivingOnBike(struct PlayState* play, struct Player* player);
// Called while the bike updates: records where it is so it is still there when Link comes back.
void MotoLink_TrackBike(struct PlayState* play, struct Actor* bike, bool ridden);
// Gives Link the key fob (MotoFob.cpp).
void MotoLink_GiveKeyFob(void);
// Ingo's races (and the escape that follows) keep using the bike if the first race was accepted on it. Set by
// En_In when the race is accepted; cleared when Link leaves Lon Lon Ranch.
void MotoLink_SetRacingOnBike(bool racingOnBike);
// Applies Link's bike riding pose to one limb (Player's limb-draw callback; MotoRider.c).
// Records the feet's limb axes (world space) as Link is drawn: gMotoRiderFootAxes[foot][0] is the ankle,
// [1..3] the ankle plus 10 units along the foot limb's local X, Y and Z (foot 0 left, 1 right). Read by the test
// driver's pose fitter.
void MotoLink_RiderPostLimb(struct Player* player, s32 limbIndex);
extern Vec3f gMotoRiderFootAxes[2][4];
void MotoLink_RiderLimbOverride(struct PlayState* play, struct Player* player, s32 limbIndex, Vec3f* pos, Vec3s* rot);
// True while Link is riding the bike (rather than Epona or nothing).
bool MotoLink_PlayerOnBike(struct PlayState* play);
// For scripted spawns that put Link on "his horse" (race start/finish, the ranch escape): ENHORSE_PARAM_MOTO if
// he was on the bike when the scene changed, else 0.
s32 MotoLink_MountParamsFlag(void);

#ifdef __cplusplus
}
#endif

#endif
