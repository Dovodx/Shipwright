#ifndef Z_EN_HORSE_MOTO_H
#define Z_EN_HORSE_MOTO_H

#include "z_en_horse.h"

// MotoLink: HORSE_MOTO-specific behaviour for En_Horse (see z_en_horse_moto.c).

// Speeds are units/frame at SoH's 20 Hz. Epona gallops at 8 and spurs to 14; the bike tops out at ~2x her spur
// (still well below an authentic Moto Racer top speed so actors keep up and walls don't tunnel).
#define MOTO_TOP_SPEED 28.8f
// Top speed with the player's setting applied (gMotoLink.Bike.TopSpeed, a multiplier).
#define MOTO_TOP_SPEED_SET (MOTO_TOP_SPEED * CVarGetFloat("gMotoLink.Bike.TopSpeed", 1.0f))
// Real-world speed scale (tachometer readout, crash thresholds): 24 units/frame is Moto Racer bike #2's 277 km/h
// (172 mph), so the bike's 28.8 reads as ~206 mph.
#define MOTO_UNITS_PER_KMH (24.0f / 277.0f)
#define MOTO_KMH(kmh) ((kmh) * MOTO_UNITS_PER_KMH)

// Analog trigger state for controller 1, 0..1, written each frame by MotoLink_FilterInput (MotoInput.cpp).
extern f32 gMotoThrottle;
extern f32 gMotoBrake;
// True while the player rides a HORSE_MOTO; the input filter then strips R/Z from the game's input.
extern u8 gMotoRiding;
// The bike the player is riding, or NULL.
extern EnHorse* gMotoRiddenBike;

void EnHorseMoto_Init(EnHorse* this, PlayState* play);
void EnHorseMoto_Destroy(EnHorse* this, PlayState* play);
void EnHorseMoto_InitColliders(EnHorse* this);
void EnHorseMoto_PlaceColliders(EnHorse* this);
void EnHorseMoto_UpdateStick(EnHorse* this, PlayState* play);
void EnHorseMoto_UpdateSpeed(EnHorse* this, PlayState* play, f32 decel);
void EnHorseMoto_UpdateRiderPos(EnHorse* this);
void EnHorseMoto_Draw(EnHorse* this, PlayState* play);
void EnHorseMoto_DrawTach(PlayState* play);
void EnHorseMoto_AdjustView(PlayState* play);
s32 EnHorseMoto_CheckWallCrash(EnHorse* this, PlayState* play);
void EnHorseMoto_ApplyRiderTilt(EnHorse* this, Player* player);
s32 EnHorseMoto_PrepareJump(EnHorse* this, f32 obstacleDist, f32 obstacleHeight);
void EnHorseMoto_StartJump(EnHorse* this, PlayState* play, s32 action);
void EnHorseMoto_UpdateJump(EnHorse* this, PlayState* play);

#endif
