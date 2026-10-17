/*
 * MotoLink: 3D model of the key fob held up when Link gets it (GetItemEntry custom draw).
 *
 * Built from flat-shaded boxes in code, like the placeholder bike, so it needs no asset files: a black fob with a
 * chrome rim and red button, a ring, and a silver key blade, about 40 units long like other get-item models.
 */
#include "global.h"
#include "soh/Enhancements/item-tables/ItemTableTypes.h"

typedef struct {
    s8 x0, y0, z0, x1, y1, z1;
    u8 r, g, b;
} FobBox;

// Model space: the key's long axis runs along +Y (the hold pose shows items upright), facing +Z.
static const FobBox sFobBoxes[] = {
    { -9, -20, -4, 9, -2, 4, 205, 208, 218 },  // chrome rim
    { -8, -19, -5, 8, -3, 5, 26, 26, 32 },     // body
    { -3, -15, 5, 3, -9, 6, 210, 30, 30 },     // button
    { -3, -2, -1, 3, 4, 1, 200, 200, 210 },    // ring
    { -2, 4, -1, 2, 22, 1, 176, 182, 196 },    // blade
    { 2, 9, -1, 4, 11, 1, 176, 182, 196 },     // teeth
    { 2, 13, -1, 4, 15, 1, 176, 182, 196 },
    { 2, 17, -1, 4, 19, 1, 176, 182, 196 },
};

static Vtx sFobVtx[ARRAY_COUNT(sFobBoxes)][24];
static u8 sFobBuilt = false;

static void Fob_BuildBox(Vtx* v, const FobBox* b) {
    static const s8 sFaces[6][4][3] = {
        { { 0, 1, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 0, 1, 1 } }, { { 0, 0, 1 }, { 1, 0, 1 }, { 1, 0, 0 }, { 0, 0, 0 } },
        { { 0, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 0, 1 } }, { { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 }, { 0, 0, 0 } },
        { { 1, 0, 1 }, { 1, 1, 1 }, { 1, 1, 0 }, { 1, 0, 0 } }, { { 0, 0, 0 }, { 0, 1, 0 }, { 0, 1, 1 }, { 0, 0, 1 } },
    };
    static const s32 sShade[6] = { 100, 45, 90, 60, 75, 75 };
    s32 f, i;

    for (f = 0; f < 6; f++) {
        for (i = 0; i < 4; i++) {
            const s8* c = sFaces[f][i];
            Vtx* vtx = &v[f * 4 + i];
            vtx->v.ob[0] = c[0] ? b->x1 : b->x0;
            vtx->v.ob[1] = c[1] ? b->y1 : b->y0;
            vtx->v.ob[2] = c[2] ? b->z1 : b->z0;
            vtx->v.flag = 0;
            vtx->v.tc[0] = vtx->v.tc[1] = 0;
            vtx->v.cn[0] = b->r * sShade[f] / 100;
            vtx->v.cn[1] = b->g * sShade[f] / 100;
            vtx->v.cn[2] = b->b * sShade[f] / 100;
            vtx->v.cn[3] = 255;
        }
    }
}

void MotoLink_DrawGiKeyFob(PlayState* play, GetItemEntry* getItemEntry) {
    s32 i, f;

    if (!sFobBuilt) {
        for (i = 0; i < ARRAY_COUNT(sFobBoxes); i++) {
            Fob_BuildBox(sFobVtx[i], &sFobBoxes[i]);
        }
        sFobBuilt = true;
    }

    OPEN_DISPS(play->state.gfxCtx);
    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_SHADE, G_CC_SHADE);
    gSPClearGeometryMode(POLY_OPA_DISP++, G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR);
    gSPSetGeometryMode(POLY_OPA_DISP++, G_CULL_BACK | G_SHADE);
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_MODELVIEW | G_MTX_LOAD);
    for (i = 0; i < ARRAY_COUNT(sFobBoxes); i++) {
        gSPVertex(POLY_OPA_DISP++, (uintptr_t)sFobVtx[i], 24, 0);
        for (f = 0; f < 6; f++) {
            gSP2Triangles(POLY_OPA_DISP++, f * 4, f * 4 + 1, f * 4 + 2, 0, f * 4, f * 4 + 2, f * 4 + 3, 0);
        }
    }
    CLOSE_DISPS(play->state.gfxCtx);
}
