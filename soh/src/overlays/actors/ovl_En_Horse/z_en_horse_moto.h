#ifndef Z_EN_HORSE_MOTO_H
#define Z_EN_HORSE_MOTO_H

#include "z_en_horse.h"

// MotoLink: HORSE_MOTO-specific behaviour for En_Horse (see z_en_horse_moto.c).

// Analog trigger state for controller 1, 0..1, written each frame by MotoLink_FilterInput (MotoInput.cpp).
extern f32 gMotoThrottle;
extern f32 gMotoBrake;
// True while the player rides a HORSE_MOTO; the input filter then strips R/Z from the game's input.
extern u8 gMotoRiding;
// The bike the player is riding, or NULL.
extern EnHorse* gMotoRiddenBike;

void EnHorseMoto_Init(EnHorse* this, PlayState* play);
void EnHorseMoto_Destroy(EnHorse* this, PlayState* play);
void EnHorseMoto_UpdateStick(EnHorse* this, PlayState* play);
void EnHorseMoto_UpdateSpeed(EnHorse* this, PlayState* play, f32 decel);
void EnHorseMoto_UpdateRiderPos(EnHorse* this);
void EnHorseMoto_Draw(EnHorse* this, PlayState* play);
void EnHorseMoto_DrawTach(PlayState* play);
void EnHorseMoto_AdjustView(PlayState* play);

#endif
