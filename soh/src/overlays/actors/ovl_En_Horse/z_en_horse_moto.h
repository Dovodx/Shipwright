#ifndef Z_EN_HORSE_MOTO_H
#define Z_EN_HORSE_MOTO_H

#include "z_en_horse.h"

// MotoLink: HORSE_MOTO-specific behaviour for En_Horse (see z_en_horse_moto.c).
void EnHorseMoto_UpdateRiderPos(EnHorse* this);
void EnHorseMoto_Draw(EnHorse* this, PlayState* play);

#endif
