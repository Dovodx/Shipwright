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
#include "textures/parameter_static/parameter_static.h"
#include "soh/OTRGlobals.h"

Gfx* Gfx_TextureI8(Gfx* displayListHead, void* texture, s16 textureWidth, s16 textureHeight, s16 rectLeft, s16 rectTop,
                   s16 rectWidth, s16 rectHeight, u16 dsdx, u16 dtdy);

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

f32 gMotoThrottle;
f32 gMotoBrake;
u8 gMotoRiding;

// Speeds are units/frame at SoH's 20 Hz. Epona gallops at 8 and spurs to 14; the bike tops out at ~1.7x her spur
// (design: cap well below an authentic Moto Racer top speed so actors keep up and walls don't tunnel).
#define MOTO_TOP_SPEED 24.0f
// Drive force at full throttle. With drag growing as (v/top)^2 this reaches 65% of top speed in ~3 s, the feel
// of Moto Racer's bike #2 (strong acceleration that tails off; see re/NOTES.md "Engine drive-force curve").
#define MOTO_ACCEL 0.32f
#define MOTO_DRAG MOTO_ACCEL // equal to MOTO_ACCEL so full throttle balances exactly at top speed
#define MOTO_ENGINE_BRAKE 0.08f
#define MOTO_BRAKE_DECEL 0.9f
// Turn rate (binary angle units/frame) at a standstill-ish crawl and at top speed; bikes turn by leaning, so the
// rate falls with speed and fades to zero below MOTO_TURN_MIN_SPEED (no pivoting on the spot).
#define MOTO_TURN_SLOW 0x300
#define MOTO_TURN_FAST 0xC0
#define MOTO_TURN_MIN_SPEED 2.0f

f32 EnHorse_SlopeSpeedMultiplier(EnHorse* this, PlayState* play);
s32 EnHorse_PlayerCanMove(EnHorse* this, PlayState* play);

EnHorse* gMotoRiddenBike;

// Engine model: Moto Racer's automatic-gearbox tachometer logic (re/NOTES.md "Engine and tachometer"), with
// bike #2's redline. RPM is cosmetic, derived from speed as a fraction of top speed.
#define MOTO_REDLINE 12500.0f
#define MOTO_IDLE_RPM 800.0f
#define MOTO_NUM_GEARS 6
static const f32 sMotoGearUpshift[MOTO_NUM_GEARS - 1] = { 0.18f, 0.33f, 0.48f, 0.62f, 0.74f };
static const f32 sMotoGearTopFrac[MOTO_NUM_GEARS] = { 0.23f, 0.38f, 0.53f, 0.67f, 0.79f, 1.0f };

s32 MotoAudio_AllocVoice(void);
void MotoAudio_FreeVoice(s32 voice);
void MotoAudio_SetVoice(s32 voice, f32 rpm, f32 load, f32 gainL, f32 gainR);

// Engine audio: distance at which a bike is at half volume, and the (deliberately low, cartoonish) speed of sound
// for Doppler, in units/frame. At 100, a bike passing at top speed (24) bends pitch by about +-25%.
#define MOTO_AUDIO_HALF_DIST 250.0f
#define MOTO_SPEED_OF_SOUND 100.0f

void EnHorseMoto_Init(EnHorse* this, PlayState* play) {
    this->motoVoice = MotoAudio_AllocVoice();
    this->motoRpm = MOTO_IDLE_RPM;
    this->motoGear = 1;
    this->motoDoppler = 1.0f;
}

void EnHorseMoto_Destroy(EnHorse* this, PlayState* play) {
    MotoAudio_FreeVoice(this->motoVoice);
    this->motoVoice = -1;
    if (gMotoRiddenBike == this) {
        gMotoRiding = false;
        gMotoRiddenBike = NULL;
    }
}

// Publishes this bike's engine voice: RPM (Doppler-shifted unless ridden), throttle load and stereo gains.
static void EnHorseMoto_UpdateEngineAudio(EnHorse* this, PlayState* play) {
    static Vec3f sPrevEye;
    static u32 sPrevEyeFrame;
    Vec3f eye = play->view.eye;
    Vec3f toEye;
    Vec3f eyeVel = { 0.0f, 0.0f, 0.0f };
    f32 dist;
    f32 gain;
    f32 pan;
    f32 doppler = 1.0f;

    // Camera velocity, from the eye's movement since the previous frame.
    if (sPrevEyeFrame + 1 == play->gameplayFrames) {
        eyeVel.x = eye.x - sPrevEye.x;
        eyeVel.y = eye.y - sPrevEye.y;
        eyeVel.z = eye.z - sPrevEye.z;
    }
    if (sPrevEyeFrame != play->gameplayFrames) {
        sPrevEye = eye;
        sPrevEyeFrame = play->gameplayFrames;
    }

    toEye.x = eye.x - this->actor.world.pos.x;
    toEye.y = eye.y - this->actor.world.pos.y;
    toEye.z = eye.z - this->actor.world.pos.z;
    dist = sqrtf(SQ(toEye.x) + SQ(toEye.y) + SQ(toEye.z));

    if (this->playerControlled) {
        gain = 1.0f; // the rider's own engine stays up front
    } else {
        gain = 1.0f / (1.0f + SQ(dist / MOTO_AUDIO_HALF_DIST));
        if (dist > 1.0f) {
            // Doppler: f' = f (c + v_listener) / (c - v_source), velocities positive when closing.
            f32 vs = (Math_SinS(this->actor.world.rot.y) * this->actor.speedXZ * toEye.x +
                      this->actor.velocity.y * toEye.y + Math_CosS(this->actor.world.rot.y) * this->actor.speedXZ * toEye.z) /
                     dist;
            f32 vl = -(eyeVel.x * toEye.x + eyeVel.y * toEye.y + eyeVel.z * toEye.z) / dist;
            doppler = CLAMP((MOTO_SPEED_OF_SOUND + vl) / (MOTO_SPEED_OF_SOUND - vs), 0.6f, 1.6f);
        }
    }
    this->motoDoppler += (doppler - this->motoDoppler) * 0.4f;

    if (play->pauseCtx.state != 0 || play->gameOverCtx.state != GAMEOVER_INACTIVE) {
        gain = 0.0f;
    }

    // Equal-power pan from the bike's on-screen position.
    pan = (this->actor.projectedW > 1.0f) ? CLAMP(this->actor.projectedPos.x / this->actor.projectedW, -1.0f, 1.0f)
                                          : 0.0f;
    pan *= 0.8f;
    MotoAudio_SetVoice(this->motoVoice, this->motoRpm * this->motoDoppler,
                       this->playerControlled ? gMotoThrottle : 0.0f,
                       gain * 1.41421356f * Math_CosF((pan + 1.0f) * (M_PI / 4.0f)),
                       gain * 1.41421356f * Math_SinF((pan + 1.0f) * (M_PI / 4.0f)));
}

static void EnHorseMoto_UpdateEngine(EnHorse* this) {
    f32 frac = this->actor.speedXZ / MOTO_TOP_SPEED;
    f32 throttle = this->playerControlled ? gMotoThrottle : 0.0f;
    f32 target;
    s32 gear = 1;

    while (gear < MOTO_NUM_GEARS && frac >= sMotoGearUpshift[gear - 1]) {
        gear++;
    }
    this->motoGear = gear;

    if (this->actor.speedXZ < 0.5f) {
        // Stopped: the throttle revs the engine in neutral.
        target = MOTO_IDLE_RPM + throttle * 0.55f * (MOTO_REDLINE - MOTO_IDLE_RPM);
    } else {
        target = CLAMP(frac / sMotoGearTopFrac[gear - 1], 0.0f, 1.0f) * MOTO_REDLINE;
        target = MAX(target, MOTO_IDLE_RPM);
    }
    this->motoRpm += (target - this->motoRpm) * 0.35f;
}

// Replaces the horse's stick reading. The gait state machine (idle/walk/trot/gallop) only looks at curStick to
// decide whether the rider wants to move, so it gets a synthetic forward stick proportional to the throttle; the
// real stick X is kept for steering.
void EnHorseMoto_UpdateStick(EnHorse* this, PlayState* play) {
    f32 stickX = play->state.input[0].rel.stick_x * (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0) ? -1 : 1);

    this->motoSteer = CLAMP(stickX / 60.0f, -1.0f, 1.0f);
    this->lastStick = this->curStick;
    this->curStick.x = 0.0f;
    this->curStick.y = gMotoThrottle * 60.0f;
    gMotoRiding = this->playerControlled;
    if (this->playerControlled) {
        gMotoRiddenBike = this;
    } else if (gMotoRiddenBike == this) {
        gMotoRiddenBike = NULL;
    }
    EnHorseMoto_UpdateEngine(this);
    EnHorseMoto_UpdateEngineAudio(this, play);
}

// Replaces EnHorse_UpdateSpeed's stick-driven speed and steering for every mounted gait.
void EnHorseMoto_UpdateSpeed(EnHorse* this, PlayState* play, f32 decel) {
    f32 top;
    f32 v = this->actor.speedXZ;
    f32 turnRate;

    if (!EnHorse_PlayerCanMove(this, play)) {
        v -= decel;
        this->actor.speedXZ = MAX(v, 0.0f);
        return;
    }

    top = MOTO_TOP_SPEED * EnHorse_SlopeSpeedMultiplier(this, play);
    v += gMotoThrottle * MOTO_ACCEL - MOTO_DRAG * SQ(v / top);
    if (gMotoThrottle < 0.05f) {
        v -= MOTO_ENGINE_BRAKE;
    }
    v -= gMotoBrake * MOTO_BRAKE_DECEL;
    this->actor.speedXZ = CLAMP(v, 0.0f, top);

    turnRate = MOTO_TURN_SLOW + (MOTO_TURN_FAST - MOTO_TURN_SLOW) * (this->actor.speedXZ / MOTO_TOP_SPEED);
    turnRate *= CLAMP(this->actor.speedXZ / MOTO_TURN_MIN_SPEED, 0.0f, 1.0f);
    // Stick right = clockwise from above = decreasing yaw
    this->actor.world.rot.y -= (s16)(this->motoSteer * turnRate);
    this->actor.shape.rot.y = this->actor.world.rot.y;
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

/*
 * Tachometer HUD (replaces Epona's carrots while riding). Vector-style, built from untextured triangles so it
 * needs no art assets: dark dial, white ticks every 1000 rpm, red zone, orange needle; gear and speed use the
 * game's counter digits. Needle sweep follows Moto Racer's tach: 215 degrees from 0 rpm to the top of the scale.
 */
#define TACH_RADIUS 24.0f
#define TACH_CENTER_X 44   // 320x240 HUD space, measured from the left edge (widescreen-adjusted)
#define TACH_CENTER_Y 160  // from the top; sits above the rupee counter
#define TACH_SCALE_MAX 13000.0f
#define TACH_RED_FROM 11500.0f
#define TACH_ANGLE_ZERO DEG_TO_RAD(225.0f) // 0 rpm at lower left
#define TACH_SWEEP DEG_TO_RAD(215.0f)      // clockwise to the top of the scale
#define TACH_DISC_SEGMENTS 32
// Speed readout in mph, as on the USA Moto Racer HUD: top speed shows bike #2's 172 mph.
#define TACH_MPH_PER_UNIT (172.0f / MOTO_TOP_SPEED)

#define TACH_MAX_VTX 512

typedef struct {
    Vtx* vtx;
    s32 count;
    Gfx** gfx;
} TachBuilder;

static f32 Tach_RpmAngle(f32 rpm) {
    return TACH_ANGLE_ZERO - TACH_SWEEP * CLAMP(rpm / TACH_SCALE_MAX, 0.0f, 1.0f);
}

// Emits one triangle; vertex coordinates are HUD-space with y up, origin at the dial centre.
static void Tach_Tri(TachBuilder* b, f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, u8 r, u8 g, u8 bl, u8 a) {
    Vtx* v;

    if (b->count + 3 > TACH_MAX_VTX) {
        return;
    }
    v = &b->vtx[b->count];
    EnHorseMoto_SetVtx(&v[0], (s16)x0, (s16)y0, 0, r, g, bl);
    EnHorseMoto_SetVtx(&v[1], (s16)x1, (s16)y1, 0, r, g, bl);
    EnHorseMoto_SetVtx(&v[2], (s16)x2, (s16)y2, 0, r, g, bl);
    v[0].v.cn[3] = v[1].v.cn[3] = v[2].v.cn[3] = a;
    gSPVertex((*b->gfx)++, v, 3, 0);
    gSP1Triangle((*b->gfx)++, 0, 1, 2, 0);
    b->count += 3;
}

// Annulus segment between two angles and radii, as a quad.
static void Tach_Arc(TachBuilder* b, f32 a0, f32 a1, f32 r0, f32 r1, u8 r, u8 g, u8 bl, u8 a) {
    f32 c0 = Math_CosF(a0), s0 = Math_SinF(a0), c1 = Math_CosF(a1), s1 = Math_SinF(a1);

    Tach_Tri(b, c0 * r0, s0 * r0, c0 * r1, s0 * r1, c1 * r1, s1 * r1, r, g, bl, a);
    Tach_Tri(b, c0 * r0, s0 * r0, c1 * r1, s1 * r1, c1 * r0, s1 * r0, r, g, bl, a);
}

// Thin radial bar centred on angle `ang`, `halfWidth` wide (HUD units), from r0 to r1.
static void Tach_Bar(TachBuilder* b, f32 ang, f32 r0, f32 r1, f32 halfWidth, u8 r, u8 g, u8 bl, u8 a) {
    f32 c = Math_CosF(ang), s = Math_SinF(ang);
    f32 px = -s * halfWidth, py = c * halfWidth;

    Tach_Tri(b, c * r0 + px, s * r0 + py, c * r1 + px, s * r1 + py, c * r1 - px, s * r1 - py, r, g, bl, a);
    Tach_Tri(b, c * r0 + px, s * r0 + py, c * r1 - px, s * r1 - py, c * r0 - px, s * r0 - py, r, g, bl, a);
}

static Gfx* Tach_Digits(Gfx* gfx, s32 value, s32 numDigits, s16 left, s16 top) {
    static const char* sDigitTex[] = { gCounterDigit0Tex, gCounterDigit1Tex, gCounterDigit2Tex, gCounterDigit3Tex,
                                       gCounterDigit4Tex, gCounterDigit5Tex, gCounterDigit6Tex, gCounterDigit7Tex,
                                       gCounterDigit8Tex, gCounterDigit9Tex };
    s32 i;

    for (i = numDigits - 1; i >= 0; i--) {
        gfx = Gfx_TextureI8(gfx, (void*)sDigitTex[value % 10], 8, 16, left + i * 8, top, 8, 16, 1 << 10, 1 << 10);
        value /= 10;
        if (value == 0 && i > 0) {
            break; // no leading zeros
        }
    }
    return gfx;
}

void EnHorseMoto_DrawTach(PlayState* play) {
    EnHorse* bike = gMotoRiddenBike;
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    TachBuilder b;
    Mtx* proj;
    f32 centerX;
    s32 i;
    s16 alpha = play->interfaceCtx.aAlpha;

    if (bike == NULL || !gMotoRiding || alpha == 0 || play->pauseCtx.state != 0) {
        return;
    }

    OPEN_DISPS(gfxCtx);

    b.vtx = Graph_Alloc(gfxCtx, sizeof(Vtx) * TACH_MAX_VTX);
    b.count = 0;
    b.gfx = &OVERLAY_DISP;
    proj = Graph_Alloc(gfxCtx, sizeof(Mtx));
    // HUD space: x right, y up, origin at the dial centre; 320x240 with widescreen edges like the rest of the HUD.
    centerX = OTRGetDimensionFromLeftEdge(TACH_CENTER_X);
    guOrtho(proj, -centerX, SCREEN_WIDTH - centerX, -(SCREEN_HEIGHT - TACH_CENTER_Y), TACH_CENTER_Y, -10.0f, 10.0f,
            1.0f);

    gDPPipeSync(OVERLAY_DISP++);
    gSPMatrix(OVERLAY_DISP++, proj, G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPMatrix(OVERLAY_DISP++, &gMtxClear, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gDPSetCycleType(OVERLAY_DISP++, G_CYC_1CYCLE);
    gDPSetRenderMode(OVERLAY_DISP++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(OVERLAY_DISP++, G_CC_SHADE, G_CC_SHADE);
    gSPClearGeometryMode(OVERLAY_DISP++, G_ZBUFFER | G_LIGHTING | G_CULL_BOTH | G_FOG | G_TEXTURE_GEN);
    gSPSetGeometryMode(OVERLAY_DISP++, G_SHADE | G_SHADING_SMOOTH);

    // Dial face and rim
    for (i = 0; i < TACH_DISC_SEGMENTS; i++) {
        f32 a0 = (2.0f * M_PI * i) / TACH_DISC_SEGMENTS;
        f32 a1 = (2.0f * M_PI * (i + 1)) / TACH_DISC_SEGMENTS;

        Tach_Arc(&b, a0, a1, 0.0f, TACH_RADIUS, 12, 12, 20, (u8)(alpha * 0.7f));
        Tach_Arc(&b, a0, a1, TACH_RADIUS, TACH_RADIUS + 1.5f, 200, 200, 210, (u8)alpha);
    }
    // Red zone
    for (i = 0; i < 6; i++) {
        f32 r0 = TACH_RED_FROM + (TACH_SCALE_MAX - TACH_RED_FROM) * i / 6.0f;
        f32 r1 = TACH_RED_FROM + (TACH_SCALE_MAX - TACH_RED_FROM) * (i + 1) / 6.0f;

        Tach_Arc(&b, Tach_RpmAngle(r0), Tach_RpmAngle(r1), TACH_RADIUS - 5.0f, TACH_RADIUS - 1.0f, 220, 40, 30,
                 (u8)alpha);
    }
    // Ticks every 1000 rpm, longer every 2000
    for (i = 0; i <= (s32)(TACH_SCALE_MAX / 1000.0f); i++) {
        Tach_Bar(&b, Tach_RpmAngle(i * 1000.0f), TACH_RADIUS - ((i % 2 == 0) ? 7.0f : 4.0f), TACH_RADIUS - 1.0f, 0.6f,
                 235, 235, 235, (u8)alpha);
    }
    // Needle and hub
    Tach_Bar(&b, Tach_RpmAngle(bike->motoRpm), -3.0f, TACH_RADIUS - 3.0f, 1.0f, 255, 140, 0, (u8)alpha);
    for (i = 0; i < 8; i++) {
        Tach_Arc(&b, (2.0f * M_PI * i) / 8, (2.0f * M_PI * (i + 1)) / 8, 0.0f, 2.5f, 255, 140, 0, (u8)alpha);
    }

    // Gear (in the dial, below the hub) and speed (beside the dial), with the game's counter digits.
    Gfx_SetupDL_39Overlay(gfxCtx);
    gDPSetCombineMode(OVERLAY_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
    gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 140, 0, alpha);
    {
        s16 left = OTRGetRectDimensionFromLeftEdge(TACH_CENTER_X - 4);
        OVERLAY_DISP = Tach_Digits(OVERLAY_DISP, bike->motoGear, 1, left, TACH_CENTER_Y + 4);
        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, alpha);
        OVERLAY_DISP = Tach_Digits(OVERLAY_DISP, (s32)(bike->actor.speedXZ * TACH_MPH_PER_UNIT + 0.5f), 3,
                                   OTRGetRectDimensionFromLeftEdge(TACH_CENTER_X + TACH_RADIUS + 4),
                                   TACH_CENTER_Y + 4);
    }

    CLOSE_DISPS(gfxCtx);
}
