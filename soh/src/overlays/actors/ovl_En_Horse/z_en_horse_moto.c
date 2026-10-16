/*
 * MotoLink: HORSE_MOTO support for En_Horse.
 *
 * The motorcycle reuses Epona's state machine and (undrawn) skeleton so that riding, jumping, races and
 * scene transfer keep working; this file supplies what differs: the seat position and the model.
 *
 * Model: generated low-poly sport bike (tools/bike/build_bike.py) in four rigid parts: chassis, fork (steers
 * about the raked steering axis) and two wheels (spin with ground speed). Lighting is baked into vertex colours.
 * The actor is drawn at scale 1 (not Epona's 0.01), so bike space is world units.
 */
#include "z_en_horse.h"
#include "z_en_horse_moto.h"
#include "textures/parameter_static/parameter_static.h"
#include "soh/OTRGlobals.h"
#include "soh/Enhancements/MotoLink/MotoLink.h"
#include "soh_assets.h"
#include "overlays/actors/ovl_En_Kusa/z_en_kusa.h"

Gfx* Gfx_TextureI8(Gfx* displayListHead, void* texture, s16 textureWidth, s16 textureHeight, s16 rectLeft, s16 rectTop,
                   s16 rectWidth, s16 rectHeight, u16 dsdx, u16 dtdy);

// Bike geometry in bike space (world units): +Z forward, +Y up, origin on the ground between the wheel contact
// points. Must match the generated model (assets/bike/bike_spec.json, built by tools/bike/build_bike.py).
#define MOTO_WHEEL_RADIUS 9
#define MOTO_FRONT_WHEEL_Z 22
#define MOTO_REAR_WHEEL_Z -22
#define MOTO_SEAT_Y 27
#define MOTO_SEAT_Z -7
// Steering head: the fork part's origin, and the rake of the steering axis.
#define MOTO_STEER_PIVOT_Y 32.0f
#define MOTO_STEER_PIVOT_Z 11.27f
#define MOTO_RAKE DEG_TO_RAD(25.0f)
#define MOTO_MAX_STEER DEG_TO_RAD(20.0f) // bars turn with the stick, less as speed builds
// The model's vertices are stored at 10x for precision.
#define MOTO_MODEL_SCALE 0.1f

// Where the rider sits relative to the bike origin (bike space). EnHorse's riderPos is consumed by Player with
// a fixed -27 Y adjustment that was tuned for Epona's saddle limb, so the offset is applied on top of the seat.
#define MOTO_RIDER_Y (MOTO_SEAT_Y + 3)

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

static void EnHorseMoto_UpdateLean(EnHorse* this);
static void EnHorseMoto_UpdateFallen(EnHorse* this);
static void EnHorseMoto_UpdateWheelie(EnHorse* this, PlayState* play);
static void EnHorseMoto_CheckActorCrash(EnHorse* this, PlayState* play);

// Engine model: Moto Racer's automatic-gearbox tachometer logic (re/NOTES.md "Engine and tachometer"), with
// bike #2's redline. RPM is cosmetic, derived from speed as a fraction of top speed.
#define MOTO_REDLINE 12500.0f
#define MOTO_IDLE_RPM 800.0f
#define MOTO_NUM_GEARS 6
static const f32 sMotoGearUpshift[MOTO_NUM_GEARS - 1] = { 0.18f, 0.33f, 0.48f, 0.62f, 0.74f };
static const f32 sMotoGearTopFrac[MOTO_NUM_GEARS] = { 0.23f, 0.38f, 0.53f, 0.67f, 0.79f, 1.0f };

s32 MotoAudio_AllocVoice(void);
void MotoAudio_FreeVoice(s32 voice);
void MotoAudio_SetVoice(s32 voice, f32 rpm, f32 load, f32 gainL, f32 gainR, u8 muffled);

// Engine audio: distance at which a bike is at half volume, and the (deliberately low, cartoonish) speed of sound
// for Doppler, in units/frame. At 100, a bike passing at top speed (24) bends pitch by about +-25%.
#define MOTO_AUDIO_HALF_DIST 250.0f
#define MOTO_SPEED_OF_SOUND 100.0f

void EnHorseMoto_Init(EnHorse* this, PlayState* play) {
    Actor* other;

    // There is only one bike: a newly placed one (e.g. Link put on it for a race) replaces any bike already here.
    for (other = play->actorCtx.actorLists[ACTORCAT_BG].head; other != NULL; other = other->next) {
        if (other != &this->actor && other->id == ACTOR_EN_HORSE && other->update != NULL &&
            ((EnHorse*)other)->type == HORSE_MOTO) {
            Actor_Kill(other);
        }
    }

    this->actor.room = -1; // one persistent bike per scene: never unloaded with a room
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
                       gain * 1.41421356f * Math_SinF((pan + 1.0f) * (M_PI / 4.0f)), this->motoUnderwater);
}

// Water above the bike's seat counts as submerged.
#define MOTO_SUBMERGED_DEPTH 30.0f
// Exhaust outlet in bike space (left side, rear), where underwater bubbles come from.
static const Vec3f sMotoExhaust = { -6.1f, 19.75f, -25.4f };

// Underwater: the bike drives along the bottom like on land (no buoyancy or drag). While Link rides it, the bike
// does the part of Player's water handling that riding skips: his depth, the underwater timer the air meter runs on
// (see Player_GetEnvironmentalHazard), and the muffled underwater sound.
static void EnHorseMoto_UpdateUnderwater(EnHorse* this, PlayState* play) {
    WaterBox* waterBox;
    f32 surface;
    bool hasWater = WaterBox_GetSurfaceImpl(play, &play->colCtx, this->actor.world.pos.x, this->actor.world.pos.z,
                                            &surface, &waterBox);

    this->motoUnderwater = hasWater && (surface - this->actor.world.pos.y) > MOTO_SUBMERGED_DEPTH;
    if (this->motoUnderwater) {
        this->motoWaterGrace = 20; // ~1 s at 20 Hz
    } else if (this->motoWaterGrace != 0) {
        this->motoWaterGrace--;
    }

    if (this->playerControlled) {
        Player* player = GET_PLAYER(play);
        f32 linkDepth = hasWater ? surface - player->actor.world.pos.y : -32000.0f;

        // (Link's own yDistToWater is left alone: Player's water code reacting to it would stop the ride.) His
        // swimming flags are cleared so that on dismounting Player re-evaluates the water from scratch.
        player->stateFlags1 &= ~PLAYER_STATE1_IN_WATER;
        player->stateFlags2 &= ~PLAYER_STATE2_UNDERWATER;
        if (linkDepth > player->ageProperties->unk_2C) {
            if (player->underwaterTimer < 300) {
                player->underwaterTimer++;
            }
            Audio_SetBaseFilter(0x20);
        } else if (player->underwaterTimer != 0) {
            player->underwaterTimer = 0;
            Audio_SetBaseFilter(0);
        }
    }

    // Underwater scene exits on the floor (the Gerudo Valley river's washout to Lake Hylia) are normally triggered
    // by Link touching them; riding, it is the bike that reaches the bottom. Link is washed out still on the bike.
    if (this->playerControlled && this->motoUnderwater && (this->actor.bgCheckFlags & BGCHECKFLAG_GROUND) &&
        this->actor.floorPoly != NULL && play->transitionTrigger == TRANS_TRIGGER_OFF) {
        u32 exitIndex = SurfaceType_GetSceneExitIndex(&play->colCtx, this->actor.floorPoly, this->actor.floorBgId);

        if (exitIndex != 0) {
            play->nextEntranceIndex = play->setupExitList[exitIndex - 1];
            AREG(6) = 1; // arrive riding (see MotoLink_SetupArrivingOnBike)
            gSaveContext.retainWeatherMode = 1;
            Scene_SetTransitionForNextEntrance(play);
            play->transitionTrigger = TRANS_TRIGGER_START;
        }
    }

    if (this->motoUnderwater && (play->gameplayFrames % 3) == 0) {
        Vec3f pos;
        f32 s = Math_SinS(this->actor.shape.rot.y);
        f32 c = Math_CosS(this->actor.shape.rot.y);

        pos.x = this->actor.world.pos.x + sMotoExhaust.x * c + sMotoExhaust.z * s;
        pos.y = this->actor.world.pos.y + sMotoExhaust.y;
        pos.z = this->actor.world.pos.z - sMotoExhaust.x * s + sMotoExhaust.z * c;
        EffectSsBubble_Spawn(play, &pos, 0.0f, 4.0f, 4.0f, 0.4f + 0.2f * (this->motoRpm / MOTO_REDLINE));
    }
}

/*
 * Grass: riding through grass and bushes cuts them, as a sword swing would, without slowing the bike or affecting
 * its handling. Grass is cut a little before the bike reaches it (further ahead the faster it goes), so its push
 * collider never gets to shove the bike, and whatever it drops is pulled straight to Link to be collected.
 */
#define MOTO_GRASS_REACH 35.0f    // cut radius around the bike at a standstill
#define MOTO_GRASS_LOOKAHEAD 2.0f // plus this many frames of travel
#define MOTO_DROP_RANGE 80.0f     // items this close to a fresh cut are the cut's drops
#define MOTO_DROP_WINDOW 40       // frames after a cut during which its drops are collected
#define MOTO_MAX_CUTS 8

static Vec3f sMotoCutPos[MOTO_MAX_CUTS];
static u32 sMotoCutFrame[MOTO_MAX_CUTS];
static s32 sMotoNextCut;

static void EnHorseMoto_CutGrass(EnHorse* this, PlayState* play) {
    f32 reach = MOTO_GRASS_REACH + this->actor.speedXZ * MOTO_GRASS_LOOKAHEAD;
    Vec3f ahead = this->actor.world.pos;
    Actor* actor;
    s32 i;

    // Centre the reach ahead of the bike so grass is caught before the wheels arrive.
    ahead.x += Math_SinS(this->actor.world.rot.y) * this->actor.speedXZ * MOTO_GRASS_LOOKAHEAD * 0.5f;
    ahead.z += Math_CosS(this->actor.world.rot.y) * this->actor.speedXZ * MOTO_GRASS_LOOKAHEAD * 0.5f;

    for (actor = play->actorCtx.actorLists[ACTORCAT_PROP].head; actor != NULL; actor = actor->next) {
        EnKusa* grass = (EnKusa*)actor;

        if (actor->id != ACTOR_EN_KUSA || actor->update == NULL || (actor->flags & ACTOR_FLAG_GRASS_DESTROYED) ||
            !(grass->collider.base.acFlags & AC_ON) || fabsf(actor->world.pos.y - this->actor.world.pos.y) > 50.0f ||
            Math_Vec3f_DistXZ(&ahead, &actor->world.pos) > reach) {
            continue;
        }
        grass->collider.base.acFlags |= AC_HIT; // En_Kusa cuts itself (and drops its item) on its next update
        grass->collider.base.ocFlags1 &= ~OC1_ON; // and no longer pushes the bike in the meantime
        sMotoCutPos[sMotoNextCut] = actor->world.pos;
        sMotoCutFrame[sMotoNextCut] = play->gameplayFrames;
        sMotoNextCut = (sMotoNextCut + 1) % MOTO_MAX_CUTS;
    }

    // Pull fresh drops from cut grass to Link (riding past, he'd otherwise leave them far behind).
    for (actor = play->actorCtx.actorLists[ACTORCAT_MISC].head; actor != NULL; actor = actor->next) {
        if (actor->id != ACTOR_EN_ITEM00 || actor->update == NULL) {
            continue;
        }
        for (i = 0; i < MOTO_MAX_CUTS; i++) {
            if (sMotoCutFrame[i] != 0 && play->gameplayFrames - sMotoCutFrame[i] < MOTO_DROP_WINDOW &&
                Math_Vec3f_DistXZ(&sMotoCutPos[i], &actor->home.pos) < MOTO_DROP_RANGE) {
                Player* player = GET_PLAYER(play);

                // Placed on Link with its pop-up bounce cancelled, it is picked up on its own update this frame.
                actor->world.pos = player->actor.world.pos;
                actor->velocity.x = actor->velocity.y = actor->velocity.z = 0.0f;
                actor->speedXZ = 0.0f;
                ((EnItem00*)actor)->unk_154 = 0; // no pickup delay
                break;
            }
        }
    }
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
    EnHorseMoto_UpdateUnderwater(this, play);
    EnHorseMoto_CheckActorCrash(this, play);
    EnHorseMoto_UpdateWheelie(this, play);
    EnHorseMoto_UpdateLean(this);
    EnHorseMoto_UpdateFallen(this);
    if (this->playerControlled) {
        EnHorseMoto_CutGrass(this, play);
    }
    EnHorseMoto_UpdateEngine(this);
    EnHorseMoto_UpdateEngineAudio(this, play);
    MotoLink_TrackBike(play, &this->actor, this->playerControlled);
}

// Lean: bikes turn by leaning, so the bike (and Link with it) rolls into the turn by up to MOTO_LEAN_MAX, more the
// faster it goes; at a standstill it rests tipped slightly onto Link's planted left foot.
#define MOTO_LEAN_MAX DEGF_TO_BINANG(35.0f)
#define MOTO_LEAN_FULL_SPEED 10.0f
#define MOTO_LEAN_STOPPED DEGF_TO_BINANG(-8.0f)

static void EnHorseMoto_UpdateLean(EnHorse* this) {
    s16 target;

    if (this->motoFallen != 0) {
        return; // EnHorseMoto_UpdateFallen drives it
    } else if (!this->playerControlled) {
        target = 0;
    } else if (this->actor.speedXZ < 0.5f) {
        target = MOTO_LEAN_STOPPED;
    } else {
        target = (s16)(this->motoSteer * MOTO_LEAN_MAX * CLAMP(this->actor.speedXZ / MOTO_LEAN_FULL_SPEED, 0.0f, 1.0f));
    }
    this->motoLean += (s16)((target - this->motoLean) * 0.25f);
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
    v += gMotoThrottle * MOTO_ACCEL * (this->motoWheelie != 0 ? 2.0f : 1.0f) - MOTO_DRAG * SQ(v / top);
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

/*
 * Crashes, after Moto Racer's rules (re/NOTES.md "Crash rules"). Its thresholds are in km/h for bike #2, whose
 * top speed is 277 km/h; they are scaled to MOTO_TOP_SPEED so the same fraction of top speed is needed here.
 *
 * Walls (`c` = how square-on the hit is, 1 = head-on):
 *   - in a wheelie above 20 km/h: crash
 *   - airborne: 25% crash
 *   - c > 0.98 above 100 km/h: crash; c > 0.5 above 80 km/h: 25% crash
 *   - otherwise a scrape: lose 25% of speed, and 70% more when hitting the wall at a steep angle.
 * Solid actors (trees, rocks, NPCs, enemies) follow Moto Racer's bike-vs-bike rules with the actor as a bike
 * standing still: a hard hit (above 90 km/h, or any hit in a wheelie) crashes 25% of the time, otherwise it is a
 * bump that costs 10% of speed. The dice roll once per new contact, and their chance is a CVar.
 *
 * Crashing throws Link off along the hit normal plus 0.6 up through Player's own large knockback, so he lands
 * and gets up as from any big hit (a quarter heart of damage); the bike falls onto the side it was leaning to and
 * lies there idling until it rights itself. It is not respawned anywhere: Link walks back or uses the fob.
 */
#define MOTO_KMH(kmh) ((kmh) * (MOTO_TOP_SPEED / 277.0f))
#define MOTO_CRASH_CHANCE 25         // percent, for Moto Racer's "rand & 3" rolls
#define MOTO_FLING_SCALE 0.6f        // Link's launch speed per unit of bike speed (Moto Racer: 1.0)
#define MOTO_CRASH_DAMAGE 4          // health units (16 per heart)
#define MOTO_FALLEN_FRAMES 70        // the bike lies on its side for 3.5 s, then rights itself
#define MOTO_WALL_TOUCH_BG 1
#define MOTO_WALL_TOUCH_ACTOR 2

static bool EnHorseMoto_CrashRoll(void) {
    return Rand_ZeroOne() * 100.0f < CVarGetInteger("gMotoLink.Crash.Chance", MOTO_CRASH_CHANCE);
}

// Crashes only happen while Link rides freely: not in races, cutscenes, scripted jumps or while getting on.
static bool EnHorseMoto_CanCrash(EnHorse* this, PlayState* play) {
    return this->playerControlled && this->motoFallen == 0 && CVarGetInteger("gMotoLink.Crash.Enabled", 1) &&
           !this->inRace && play->csCtx.state == CS_STATE_IDLE && play->transitionTrigger == TRANS_TRIGGER_OFF &&
           !(this->stateFlags & ENHORSE_JUMPING) && this->action != ENHORSE_ACT_FROZEN &&
           this->action != ENHORSE_ACT_LOW_JUMP && this->action != ENHORSE_ACT_HIGH_JUMP &&
           this->action != ENHORSE_ACT_BRIDGE_JUMP && !Player_InBlockingCsMode(play, GET_PLAYER(play));
}

static void EnHorseMoto_Crash(EnHorse* this, PlayState* play, s16 normalYaw) {
    Player* player = GET_PLAYER(play);
    f32 speed = this->actor.speedXZ;
    f32 fling = speed * CVarGetFloat("gMotoLink.Crash.Fling", MOTO_FLING_SCALE);

    // Off the bike, the same way Player's dismount ends (Player_Action_8084D3E4), but mid-ride.
    this->actor.child = NULL;
    player->actor.parent = NULL;
    player->stateFlags1 &= ~PLAYER_STATE1_ON_HORSE;
    AREG(6) = 0;
    Camera_RequestSetting(Play_GetCamera(play, CAM_ID_MAIN), CAM_SET_NORMAL0);

    // Launch along normal + 0.6 up, normalised: 0.857 of the speed outwards, 0.514 upwards. Player picks the
    // knockback up on its own update this frame (it runs after the bike's) now that Link has no parent.
    Actor_SetPlayerKnockbackLarge(play, &this->actor, fling * 0.857f, normalYaw, fling * 0.514f,
                                  speed > MOTO_KMH(20.0f) ? MOTO_CRASH_DAMAGE : 0);

    // The bike drops onto the side it leant to (either side when upright) and stops dead.
    if (ABS(this->motoLean) > DEGF_TO_BINANG(3.0f)) {
        this->motoFallen = (this->motoLean > 0) ? 0x4000 : -0x4000;
    } else {
        this->motoFallen = (Rand_ZeroOne() < 0.5f) ? 0x4000 : -0x4000;
    }
    this->motoFallenTimer = MOTO_FALLEN_FRAMES;
    this->motoWheelie = 0;
    this->actor.speedXZ = 0.0f;
    Player_PlaySfx(&this->actor, NA_SE_IT_WALL_HIT_HARD);
    Player_PlaySfx(&this->actor, NA_SE_EV_STONE_BOUND);
}

// Wall rules, run from EnHorse_UpdateBgCheckInfo right after the bike's BgCheck. Returns true on a crash.
s32 EnHorseMoto_CheckWallCrash(EnHorse* this, PlayState* play) {
    bool touching = (this->actor.bgCheckFlags & BGCHECKFLAG_WALL) != 0;
    bool newContact = touching && !(this->motoWallTouch & MOTO_WALL_TOUCH_BG);
    f32 v = this->actor.speedXZ;
    f32 into;
    bool crash;

    this->motoWallTouch = (this->motoWallTouch & ~MOTO_WALL_TOUCH_BG) | (touching ? MOTO_WALL_TOUCH_BG : 0);
    if (!touching || !this->playerControlled) {
        return false;
    }
    // wallYaw is the wall normal's direction (out of the wall), so heading into it gives a negative cosine.
    into = -Math_CosS(this->actor.wallYaw - this->actor.world.rot.y);
    if (into < 0.1f) {
        return false; // sliding along it or moving away
    }

    if (!EnHorseMoto_CanCrash(this, play)) {
        crash = false;
    } else if (this->motoWheelie != 0) {
        crash = v > MOTO_KMH(20.0f);
    } else if (!(this->actor.bgCheckFlags & BGCHECKFLAG_GROUND)) {
        crash = newContact && v > MOTO_KMH(20.0f) && EnHorseMoto_CrashRoll();
    } else {
        crash = (into > 0.98f && v > MOTO_KMH(100.0f)) ||
                (newContact && into > 0.5f && v > MOTO_KMH(80.0f) && EnHorseMoto_CrashRoll());
    }
    if (crash) {
        EnHorseMoto_Crash(this, play, this->actor.wallYaw);
        return true;
    }

    // Scrape
    if (newContact) {
        v *= 0.75f;
        if (into > 0.5f) {
            v *= 0.3f;
        }
        if (this->actor.speedXZ > MOTO_KMH(20.0f)) {
            Player_PlaySfx(&this->actor, NA_SE_IT_WALL_HIT_HARD);
        }
    } else if (into > 0.3f && v > 4.0f) {
        v -= 1.0f; // held against the wall
    }
    this->actor.speedXZ = v;
    return false;
}

// Bike-vs-solid-actor rules, from the bike's colliders' overlap results of the previous frame.
static bool EnHorseMoto_IsSolid(Actor* other) {
    if (other == NULL || other->update == NULL || other->id == ACTOR_EN_KUSA || other->id == ACTOR_EN_HORSE) {
        return false;
    }
    switch (other->category) {
        case ACTORCAT_BG:
        case ACTORCAT_PROP:
        case ACTORCAT_NPC:
        case ACTORCAT_ENEMY:
        case ACTORCAT_BOSS:
            return true;
        default:
            return false;
    }
}

static void EnHorseMoto_CheckActorCrash(EnHorse* this, PlayState* play) {
    Collider* colliders[] = { &this->cyl1.base, &this->cyl2.base, &this->jntSph.base };
    Actor* other = NULL;
    bool touching;
    bool newContact;
    s32 i;

    for (i = 0; i < ARRAY_COUNT(colliders); i++) {
        if ((colliders[i]->ocFlags1 & OC1_HIT) && EnHorseMoto_IsSolid(colliders[i]->oc)) {
            other = colliders[i]->oc;
            break;
        }
    }
    touching = other != NULL;
    newContact = touching && !(this->motoWallTouch & MOTO_WALL_TOUCH_ACTOR);
    this->motoWallTouch = (this->motoWallTouch & ~MOTO_WALL_TOUCH_ACTOR) | (touching ? MOTO_WALL_TOUCH_ACTOR : 0);

    if (newContact && EnHorseMoto_CanCrash(this, play)) {
        s16 normalYaw = Math_Vec3f_Yaw(&other->world.pos, &this->actor.world.pos);
        f32 into = -Math_CosS(normalYaw - this->actor.world.rot.y);
        f32 v = this->actor.speedXZ;

        if (into < 0.1f) {
            return; // brushing past
        }
        if ((v > MOTO_KMH(90.0f) || (this->motoWheelie != 0 && v > MOTO_KMH(20.0f))) && EnHorseMoto_CrashRoll()) {
            EnHorseMoto_Crash(this, play, normalYaw);
        } else {
            this->actor.speedXZ = v * 0.9f;
        }
    }
}

// A fallen bike lies on its side, then rights itself.
static void EnHorseMoto_UpdateFallen(EnHorse* this) {
    if (this->motoFallen == 0) {
        return;
    }
    this->actor.speedXZ = 0.0f;
    if (this->motoFallenTimer > 0) {
        this->motoFallenTimer--;
        Math_ScaledStepToS(&this->motoLean, this->motoFallen, 0x1000);
    } else if (Math_ScaledStepToS(&this->motoLean, 0, 0x300)) {
        this->motoFallen = 0;
    }
}

/*
 * Wheelie (Moto Racer's L2, here the stick pulled back on the throttle): the front wheel comes up in about a third
 * of a second and stays up as long as it is held (no time limit, unlike Moto Racer's 2 s), and while it is up the
 * drive force doubles. Touching a wall above 20 km/h in a wheelie is a guaranteed crash.
 */
#define MOTO_WHEELIE_MAX DEGF_TO_BINANG(40.0f)
#define MOTO_WHEELIE_RISE (MOTO_WHEELIE_MAX / 7)  // ~0.33 s up
#define MOTO_WHEELIE_FALL (MOTO_WHEELIE_MAX / 12) // ~0.6 s down
#define MOTO_WHEELIE_STICK -40

static void EnHorseMoto_UpdateWheelie(EnHorse* this, PlayState* play) {
    bool held = this->playerControlled && this->motoFallen == 0 && gMotoThrottle > 0.5f && gMotoBrake < 0.1f &&
                play->state.input[0].rel.stick_y < MOTO_WHEELIE_STICK && this->actor.speedXZ > 1.0f &&
                ((this->actor.bgCheckFlags & BGCHECKFLAG_GROUND) || this->motoWheelie != 0);
    s16 max = DEGF_TO_BINANG(CVarGetFloat("gMotoLink.Wheelie.Angle", BINANG_TO_DEGF(MOTO_WHEELIE_MAX)));

    if (held) {
        this->motoWheelie = MIN(this->motoWheelie + MOTO_WHEELIE_RISE, max);
    } else {
        this->motoWheelie = MAX(this->motoWheelie - MOTO_WHEELIE_FALL, 0);
    }
}

// Rider tilt, applied at Link's root limb (MotoRider.c): the bike's lean (a roll about the ground line under the
// seat, which is Link's origin) and the wheelie (a pitch about the rear axle), so he stays on the seat.
void EnHorseMoto_ApplyRiderTilt(EnHorse* this, Player* player) {
    Matrix_RotateZ(BINANG_TO_RAD(-this->motoLean), MTXMODE_APPLY);
    if (this->motoWheelie != 0) {
        // Rear axle in Link's model space: his origin is riderPos.y - 27 above the bike's, at the seat.
        f32 toModel = 1.0f / player->actor.scale.y;
        f32 axleY = (MOTO_WHEEL_RADIUS - (this->riderPos.y - 27.0f)) * toModel;
        f32 axleZ = (MOTO_REAR_WHEEL_Z - MOTO_SEAT_Z) * toModel;

        Matrix_Translate(0.0f, axleY, axleZ, MTXMODE_APPLY);
        Matrix_RotateX(BINANG_TO_RAD(-this->motoWheelie), MTXMODE_APPLY);
        Matrix_Translate(0.0f, -axleY, -axleZ, MTXMODE_APPLY);
    }
}

/*
 * Bike camera, applied to the view just before drawing (the camera recomputes the view every frame, so this never
 * accumulates): a wider field of view the faster the bike goes, and the eye brought in and lowered for a tighter,
 * lower chase view than Epona's.
 */
#define MOTO_CAM_FOV_KICK 10.0f  // extra degrees at top speed
#define MOTO_CAM_PULLBACK 0.8f   // eye distance multiplier (closer chase view than Epona's)
#define MOTO_CAM_LOWER 6.0f      // eye lowered by this many units

void EnHorseMoto_AdjustView(PlayState* play) {
    EnHorse* bike = gMotoRiddenBike;
    View* view = &play->view;
    f32 speedFrac;

    if (bike == NULL || !gMotoRiding || play->activeCamera != CAM_ID_MAIN) {
        return;
    }
    speedFrac = CLAMP(bike->actor.speedXZ / MOTO_TOP_SPEED, 0.0f, 1.0f);
    view->fovy += CVarGetFloat("gMotoLink.Camera.FovKick", MOTO_CAM_FOV_KICK) * speedFrac * speedFrac;
    {
        f32 pull = CVarGetFloat("gMotoLink.Camera.Pullback", MOTO_CAM_PULLBACK);
        view->eye.x = view->lookAt.x + (view->eye.x - view->lookAt.x) * pull;
        view->eye.z = view->lookAt.z + (view->eye.z - view->lookAt.z) * pull;
        view->eye.y = view->lookAt.y + (view->eye.y - view->lookAt.y) * pull - CVarGetFloat("gMotoLink.Camera.Lower", MOTO_CAM_LOWER);
    }
}

void EnHorseMoto_UpdateRiderPos(EnHorse* this) {
    f32 s = Math_SinS(this->actor.shape.rot.y);
    f32 c = Math_CosS(this->actor.shape.rot.y);

    this->riderPos.x = s * MOTO_SEAT_Z;
    this->riderPos.y = CVarGetFloat("gMotoLink.Pose.SeatY", MOTO_RIDER_Y);
    this->riderPos.z = c * MOTO_SEAT_Z;
}

void EnHorseMoto_Draw(EnHorse* this, PlayState* play) {
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

    Gfx_SetupDL_25Opa(play->state.gfxCtx); // the part display lists set their own (unlit, vertex colour) modes

    Matrix_SetTranslateRotateYXZ(this->actor.world.pos.x, this->actor.world.pos.y, this->actor.world.pos.z,
                                 &this->actor.shape.rot);
    Matrix_RotateZ(BINANG_TO_RAD(-this->motoLean), MTXMODE_APPLY); // roll about the ground contact line
    if (this->motoWheelie != 0) { // pitch about the rear axle
        Matrix_Translate(0.0f, MOTO_WHEEL_RADIUS, MOTO_REAR_WHEEL_Z, MTXMODE_APPLY);
        Matrix_RotateX(BINANG_TO_RAD(-this->motoWheelie), MTXMODE_APPLY);
        Matrix_Translate(0.0f, -MOTO_WHEEL_RADIUS, -MOTO_REAR_WHEEL_Z, MTXMODE_APPLY);
    }

    // Chassis
    Matrix_Push();
    Matrix_Scale(MOTO_MODEL_SCALE, MOTO_MODEL_SCALE, MOTO_MODEL_SCALE, MTXMODE_APPLY);
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gMotoBikeChassisDL);
    Matrix_Pop();

    // Rear wheel
    Matrix_Push();
    Matrix_Translate(0.0f, MOTO_WHEEL_RADIUS, MOTO_REAR_WHEEL_Z, MTXMODE_APPLY);
    Matrix_RotateX(BINANG_TO_RAD(this->motoWheelRot), MTXMODE_APPLY);
    Matrix_Scale(MOTO_MODEL_SCALE, MOTO_MODEL_SCALE, MOTO_MODEL_SCALE, MTXMODE_APPLY);
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gMotoBikeWheelRearDL);
    Matrix_Pop();

    // Fork and front wheel, turned about the raked steering axis (steering right = clockwise from above).
    {
        f32 steer = -this->motoSteer * MOTO_MAX_STEER /
                    (1.0f + CLAMP(this->actor.speedXZ, 0.0f, MOTO_TOP_SPEED) * (3.0f / MOTO_TOP_SPEED));

        if (!this->playerControlled) {
            steer = 0.0f;
        }
        Matrix_Translate(0.0f, MOTO_STEER_PIVOT_Y, MOTO_STEER_PIVOT_Z, MTXMODE_APPLY);
        Matrix_RotateX(-MOTO_RAKE, MTXMODE_APPLY);
        Matrix_RotateY(steer, MTXMODE_APPLY);
        Matrix_RotateX(MOTO_RAKE, MTXMODE_APPLY);
        Matrix_Push();
        Matrix_Scale(MOTO_MODEL_SCALE, MOTO_MODEL_SCALE, MOTO_MODEL_SCALE, MTXMODE_APPLY);
        gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gMotoBikeForkDL);
        Matrix_Pop();

        Matrix_Translate(0.0f, MOTO_WHEEL_RADIUS - MOTO_STEER_PIVOT_Y, MOTO_FRONT_WHEEL_Z - MOTO_STEER_PIVOT_Z,
                         MTXMODE_APPLY);
        Matrix_RotateX(BINANG_TO_RAD(this->motoWheelRot), MTXMODE_APPLY);
        Matrix_Scale(MOTO_MODEL_SCALE, MOTO_MODEL_SCALE, MOTO_MODEL_SCALE, MTXMODE_APPLY);
        gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gMotoBikeWheelFrontDL);
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
        OVERLAY_DISP = Tach_Digits(OVERLAY_DISP, (s32)(fabsf(bike->actor.speedXZ) * TACH_MPH_PER_UNIT + 0.5f), 3,
                                   OTRGetRectDimensionFromLeftEdge(TACH_CENTER_X + TACH_RADIUS + 4),
                                   TACH_CENTER_Y + 4);
    }

    CLOSE_DISPS(gfxCtx);
}
