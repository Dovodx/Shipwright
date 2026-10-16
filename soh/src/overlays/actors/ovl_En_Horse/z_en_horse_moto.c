/*
 * MotoLink: HORSE_MOTO support for En_Horse.
 *
 * The motorcycle reuses Epona's state machine and (undrawn) skeleton so that riding, jumping, races and
 * scene transfer keep working; this file supplies what differs: the seat position and the model.
 *
 * Placeholder model: flat-shaded boxes and octagonal wheels built from vertex colours, in world units
 * (the actor is drawn at scale 1, not Epona's 0.01). +Z is forward, +Y is up, origin is on the ground
 * between the wheels. Proportions are for adult Link (~1 unit = 3.2 cm): 64-unit wheelbase, 9-unit-radius
 * wheels, seat at 27.
 */
#include "z_en_horse.h"
#include "z_en_horse_moto.h"

#define MOTO_WHEEL_RADIUS 9
#define MOTO_WHEEL_HALF_WIDTH 3
#define MOTO_FRONT_WHEEL_Z 22
#define MOTO_REAR_WHEEL_Z -20
#define MOTO_SEAT_Y 27
#define MOTO_SEAT_Z -6

// Where the rider sits relative to the bike origin (bike space). EnHorse's riderPos is consumed by Player with
// a fixed -27 Y adjustment that was tuned for Epona's saddle limb, so the offset is applied on top of the seat.
#define MOTO_RIDER_Y (MOTO_SEAT_Y + 18)

typedef struct {
    s16 x0, y0, z0, x1, y1, z1;
    u8 r, g, b;
} MotoBox;

static const MotoBox sMotoBodyBoxes[] = {
    { -5, 13, -14, 5, 26, 16, 200, 30, 30 },   // tank + fairing
    { -6, 26, -18, 6, 29, 2, 25, 25, 25 },     // seat
    { -4, 18, -24, -1, 24, -12, 120, 120, 130 }, // exhaust (left)
    { -2, 9, 14, 2, 36, 18, 150, 150, 160 },   // fork
    { -11, 35, 13, -2, 38, 16, 40, 40, 40 },   // handlebar (left)
    { 2, 35, 13, 11, 38, 16, 40, 40, 40 },     // handlebar (right)
    { -6, 24, 16, 6, 34, 22, 220, 220, 230 },  // headlight cowl
    { -2, 8, -20, 2, 14, -6, 90, 90, 100 },    // swingarm
};

#define MOTO_NUM_BOXES ARRAY_COUNT(sMotoBodyBoxes)

static Vtx sMotoBoxVtx[MOTO_NUM_BOXES][24];
static Vtx sMotoWheelVtx[32];
static u8 sMotoGeometryBuilt = false;

static void EnHorseMoto_SetVtx(Vtx* v, s16 x, s16 y, s16 z, u8 r, u8 g, u8 b) {
    v->v.ob[0] = x;
    v->v.ob[1] = y;
    v->v.ob[2] = z;
    v->v.flag = 0;
    v->v.tc[0] = v->v.tc[1] = 0;
    v->v.cn[0] = r;
    v->v.cn[1] = g;
    v->v.cn[2] = b;
    v->v.cn[3] = 255;
}

static u8 EnHorseMoto_Shade(u8 c, s32 percent) {
    return (u8)((c * percent) / 100);
}

// 6 faces x 4 vertices, each face shaded by orientation so the flat colours read as a solid.
static void EnHorseMoto_BuildBox(Vtx* v, const MotoBox* b) {
    static const s8 sFaces[6][4][3] = {
        { { 0, 1, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 0, 1, 1 } }, // top
        { { 0, 0, 1 }, { 1, 0, 1 }, { 1, 0, 0 }, { 0, 0, 0 } }, // bottom
        { { 0, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 0, 1 } }, // front (+z)
        { { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 }, { 0, 0, 0 } }, // back (-z)
        { { 1, 0, 1 }, { 1, 1, 1 }, { 1, 1, 0 }, { 1, 0, 0 } }, // right (+x)
        { { 0, 0, 0 }, { 0, 1, 0 }, { 0, 1, 1 }, { 0, 0, 1 } }, // left (-x)
    };
    static const s32 sShade[6] = { 100, 45, 85, 60, 75, 75 };
    s32 f, i;

    for (f = 0; f < 6; f++) {
        for (i = 0; i < 4; i++) {
            const s8* c = sFaces[f][i];
            EnHorseMoto_SetVtx(&v[f * 4 + i], c[0] ? b->x1 : b->x0, c[1] ? b->y1 : b->y0, c[2] ? b->z1 : b->z0,
                               EnHorseMoto_Shade(b->r, sShade[f]), EnHorseMoto_Shade(b->g, sShade[f]),
                               EnHorseMoto_Shade(b->b, sShade[f]));
        }
    }
}

// Octagonal tyre around the X axis, centred on the origin: 8 rim points per side (0-7 left, 8-15 right) plus
// 8-15 duplicated as 16-31 with alternating tread colours so the spin is visible.
static void EnHorseMoto_BuildWheel(Vtx* v) {
    s32 i;

    for (i = 0; i < 8; i++) {
        f32 a = i * (M_PI / 4.0f);
        s16 y = (s16)(MOTO_WHEEL_RADIUS * Math_CosF(a));
        s16 z = (s16)(MOTO_WHEEL_RADIUS * Math_SinF(a));
        u8 tread = (i & 1) ? 70 : 30;

        EnHorseMoto_SetVtx(&v[i], -MOTO_WHEEL_HALF_WIDTH, y, z, 45, 45, 50);     // left sidewall
        EnHorseMoto_SetVtx(&v[8 + i], MOTO_WHEEL_HALF_WIDTH, y, z, 45, 45, 50);  // right sidewall
        EnHorseMoto_SetVtx(&v[16 + i], -MOTO_WHEEL_HALF_WIDTH, y, z, tread, tread, tread); // tread left edge
        EnHorseMoto_SetVtx(&v[24 + i], MOTO_WHEEL_HALF_WIDTH, y, z, tread, tread, tread);  // tread right edge
    }
}

static void EnHorseMoto_BuildGeometry(void) {
    s32 i;

    for (i = 0; i < MOTO_NUM_BOXES; i++) {
        EnHorseMoto_BuildBox(sMotoBoxVtx[i], &sMotoBodyBoxes[i]);
    }
    EnHorseMoto_BuildWheel(sMotoWheelVtx);
    sMotoGeometryBuilt = true;
}

static Gfx* EnHorseMoto_DrawBox(Gfx* gfx, Vtx* v) {
    s32 f;

    gSPVertex(gfx++, v, 24, 0);
    for (f = 0; f < 6; f++) {
        gSP2Triangles(gfx++, f * 4, f * 4 + 1, f * 4 + 2, 0, f * 4, f * 4 + 2, f * 4 + 3, 0);
    }
    return gfx;
}

static Gfx* EnHorseMoto_DrawWheel(Gfx* gfx) {
    s32 i;

    gSPVertex(gfx++, sMotoWheelVtx, 32, 0);
    // sidewalls: fans around vertex 0 / 8
    for (i = 1; i < 7; i++) {
        gSP2Triangles(gfx++, 0, i + 1, i, 0, 8, 8 + i, 8 + i + 1, 0);
    }
    // tread quads
    for (i = 0; i < 8; i++) {
        s32 j = (i + 1) & 7;
        gSP2Triangles(gfx++, 16 + i, 16 + j, 24 + j, 0, 16 + i, 24 + j, 24 + i, 0);
    }
    return gfx;
}

void EnHorseMoto_UpdateRiderPos(EnHorse* this) {
    f32 s = Math_SinS(this->actor.shape.rot.y);
    f32 c = Math_CosS(this->actor.shape.rot.y);

    this->riderPos.x = s * MOTO_SEAT_Z;
    this->riderPos.y = MOTO_RIDER_Y;
    this->riderPos.z = c * MOTO_SEAT_Z;
}

void EnHorseMoto_Draw(EnHorse* this, PlayState* play) {
    Gfx* gfx;
    s32 i;

    if (!sMotoGeometryBuilt) {
        EnHorseMoto_BuildGeometry();
    }

    // Wheel spin follows ground speed: speedXZ units/frame over a 9-unit radius.
    this->motoWheelRot += RADF_TO_BINANG(this->actor.speedXZ / MOTO_WHEEL_RADIUS);

    if (!(this->stateFlags & ENHORSE_CALC_RIDER_POS)) {
        EnHorseMoto_UpdateRiderPos(this);
    } else {
        this->stateFlags &= ~ENHORSE_CALC_RIDER_POS;
    }
    // Sound source for any horse SFX that still fire (to be replaced by engine audio).
    this->unk_228 = this->actor.projectedPos;

    OPEN_DISPS(play->state.gfxCtx);

    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_SHADE, G_CC_SHADE);
    gSPClearGeometryMode(POLY_OPA_DISP++, G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR);
    gSPSetGeometryMode(POLY_OPA_DISP++, G_CULL_BACK | G_SHADE);

    Matrix_SetTranslateRotateYXZ(this->actor.world.pos.x, this->actor.world.pos.y, this->actor.world.pos.z,
                                 &this->actor.shape.rot);
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

    gfx = POLY_OPA_DISP;
    for (i = 0; i < MOTO_NUM_BOXES; i++) {
        gfx = EnHorseMoto_DrawBox(gfx, sMotoBoxVtx[i]);
    }
    POLY_OPA_DISP = gfx;

    for (i = 0; i < 2; i++) {
        Matrix_Push();
        Matrix_Translate(0.0f, MOTO_WHEEL_RADIUS, i == 0 ? MOTO_FRONT_WHEEL_Z : MOTO_REAR_WHEEL_Z, MTXMODE_APPLY);
        Matrix_RotateX(BINANG_TO_RAD(this->motoWheelRot), MTXMODE_APPLY);
        gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        POLY_OPA_DISP = EnHorseMoto_DrawWheel(POLY_OPA_DISP);
        Matrix_Pop();
    }

    CLOSE_DISPS(play->state.gfxCtx);
}
