/*
 * MotoLink: Link's riding pose on the bike.
 *
 * Instead of new animation files, the pose is applied procedurally on top of whatever riding animation plays: while
 * Link is on the bike, the limbs listed below take fixed rotations (sitting with knees forward, hands on the bars)
 * in Player's limb-draw callback. Every value can be overridden at runtime with the console variable
 * gMotoLink.Pose.<limb>.<x|y|z> (e.g. "motoset gMotoLink.Pose.4.z -18000"; use motoset, as the console's "set"
 * crashes on negative numbers; the stopped foot-down pose uses gMotoLink.FootDown.<limb>.<axis> and the tuck
 * gMotoLink.Tuck.<limb>.<axis>), which is how the pose is tuned.
 */
#include <stdio.h>

#include "global.h"
#include "soh/Enhancements/MotoLink/MotoLink.h"
#include "overlays/actors/ovl_En_Horse/z_en_horse.h"
#include "overlays/actors/ovl_En_Horse/z_en_horse_moto.h"
#include "objects/gameplay_keep/gameplay_keep.h"
#include "soh/ResourceManagerHelpers.h"

typedef struct {
    u8 limb;
    s16 x, y, z;
} RiderJoint;

// Seated pose (limb-local rotations, binary angles), fitted in game to the generated bike (assets/bike/bike_spec.json)
// with the test driver's "fitrider" stages: head over the tank, wrists at the grips, knees against the tank,
// ankles over the pegs, feet flat on them. Refit after changing the bike's proportions.
static const RiderJoint sBikePose[] = {
    // Body held still (Epona's riding animations bob and look around; a motorbike rider sits still): the root faces
    // straight ahead and the waist keeps the riding animation's rest value.
    { PLAYER_LIMB_ROOT, 0, 0, 0 },
    { PLAYER_LIMB_WAIST, -21951, -614, -14838 },
    { PLAYER_LIMB_LOWER, 0, 0, 0 },  // parent of the legs
    { PLAYER_LIMB_COLLAR, 0, 0, 0 }, // parent of the arms (Epona's idle variants shrug it)
    { PLAYER_LIMB_UPPER, 16408, -6423, 16342 }, // leaning forward over the tank
    { PLAYER_LIMB_R_THIGH, 7683, 8916, 136 },
    { PLAYER_LIMB_R_SHIN, 0, 0, 17072 },
    { PLAYER_LIMB_R_FOOT, 3856, -2783, -25716 }, // toes forward, sole on the peg
    { PLAYER_LIMB_L_THIGH, 3112, 492, -6616 },
    { PLAYER_LIMB_L_SHIN, 0, 0, 17712 },
    { PLAYER_LIMB_L_FOOT, -2170, 1283, -24760 },
    // Arms reaching for the grips (Epona's riding animation holds reins low and to one side).
    { PLAYER_LIMB_L_SHOULDER, 15699, -3178, 20271 },
    { PLAYER_LIMB_L_FOREARM, 10420, -2432, -6914 },
    { PLAYER_LIMB_L_HAND, 3835, -7257, -22468 },
    { PLAYER_LIMB_R_SHOULDER, 1818, 29936, -14255 },
    { PLAYER_LIMB_R_FOREARM, 1739, 5720, -2295 },
    { PLAYER_LIMB_R_HAND, 2206, -1164, -13920 },
};

// Stopped: the left leg goes down to the ground, a little out to the side (the bike tips onto it).
static const RiderJoint sFootDown[] = {
    { PLAYER_LIMB_L_THIGH, -2104, -6500, -3886 },
    { PLAYER_LIMB_L_SHIN, 0, 0, 3000 },
    { PLAYER_LIMB_L_FOOT, -1722, -2045, -19000 },
};

// Tuck: with speed, the upper body and arms blend from the seated pose to this one (head down over the tank,
// hands still on the bars). Limbs not listed here keep their seated values; console variables
// gMotoLink.Tuck.<limb>.<axis> override it like the seated pose.
static const RiderJoint sTuckPose[] = {
    { PLAYER_LIMB_UPPER, 16408, -11927, 16342 },
    { PLAYER_LIMB_L_SHOULDER, 9427, -3690, 21935 },
    { PLAYER_LIMB_L_FOREARM, 10420, -6016, -19458 },
    { PLAYER_LIMB_R_SHOULDER, 12058, 27824, -11823 },
    { PLAYER_LIMB_R_FOREARM, 2059, 13912, -28663 },
};

#define RIDER_FOOT_DOWN_SPEED 0.5f

#define RIDER_SEATED_PELVIS_Y 3377.0f // root height while seated (the riding animation's rest value)

static bool Rider_AnimIs(SkelAnime* skelAnime, const char* path) {
    void* current = skelAnime->animation;

    if (current == NULL) {
        return false;
    }
    if (ResourceMgr_OTRSigCheck(current)) {
        current = ResourceMgr_LoadAnimByName(current);
    }
    return current == ResourceMgr_LoadAnimByName(path);
}

static bool Rider_IsMountingOrDismounting(Player* player) {
    return Rider_AnimIs(&player->skelAnime, dgPlayerAnim_link_uma_left_up) ||
           Rider_AnimIs(&player->skelAnime, dgPlayerAnim_link_uma_right_up) ||
           Rider_AnimIs(&player->skelAnime, dgPlayerAnim_link_uma_left_down) ||
           Rider_AnimIs(&player->skelAnime, dgPlayerAnim_link_uma_right_down);
}

static s16 Rider_Value(const char* prefix, u8 limb, char axis, s16 defaultValue) {
    char name[48];

    snprintf(name, sizeof(name), "%s.%d.%c", prefix, limb, axis);
    return (s16)CVarGetInteger(name, defaultValue);
}

static const RiderJoint* Rider_Find(const RiderJoint* pose, s32 count, s32 limbIndex) {
    s32 i;

    for (i = 0; i < count; i++) {
        if (pose[i].limb == limbIndex) {
            return &pose[i];
        }
    }
    return NULL;
}

static s16 Rider_Axis(const RiderJoint* joint, char axis) {
    return axis == 'x' ? joint->x : axis == 'y' ? joint->y : joint->z;
}

static bool Rider_Apply(const char* prefix, const RiderJoint* pose, s32 count, s32 limbIndex, Vec3s* rot) {
    const RiderJoint* joint = Rider_Find(pose, count, limbIndex);

    if (joint == NULL) {
        return false;
    }
    rot->x = Rider_Value(prefix, joint->limb, 'x', joint->x);
    rot->y = Rider_Value(prefix, joint->limb, 'y', joint->y);
    rot->z = Rider_Value(prefix, joint->limb, 'z', joint->z);
    return true;
}

// The seated (tuck = false) or tucked pose's value for one joint axis, console overrides included. A limb missing
// from the tuck table tucks to its seated value.
static s16 Rider_PoseValue(bool tuck, u8 limb, char axis) {
    const RiderJoint* seated = Rider_Find(sBikePose, ARRAY_COUNT(sBikePose), limb);
    const RiderJoint* tucked = Rider_Find(sTuckPose, ARRAY_COUNT(sTuckPose), limb);
    s16 value;

    if (seated == NULL) {
        return 0;
    }
    value = Rider_Value("gMotoLink.Pose", limb, axis, Rider_Axis(seated, axis));
    if (tuck) {
        value = Rider_Value("gMotoLink.Tuck", limb, axis, tucked != NULL ? Rider_Axis(tucked, axis) : value);
    }
    return value;
}

// Seated pose blended towards the tuck by `tuck` (0..1); s16 differences wrap, so the blend takes the short way.
static bool Rider_ApplyBlended(s32 limbIndex, f32 tuck, Vec3s* rot) {
    if (Rider_Find(sBikePose, ARRAY_COUNT(sBikePose), limbIndex) == NULL) {
        return false;
    }
    rot->x = Rider_PoseValue(false, limbIndex, 'x');
    rot->y = Rider_PoseValue(false, limbIndex, 'y');
    rot->z = Rider_PoseValue(false, limbIndex, 'z');
    if (tuck > 0.0f) {
        rot->x += (s16)((s16)(Rider_PoseValue(true, limbIndex, 'x') - rot->x) * tuck);
        rot->y += (s16)((s16)(Rider_PoseValue(true, limbIndex, 'y') - rot->y) * tuck);
        rot->z += (s16)((s16)(Rider_PoseValue(true, limbIndex, 'z') - rot->z) * tuck);
    }
    return true;
}

void MotoLink_RiderLimbOverride(PlayState* play, Player* player, s32 limbIndex, Vec3f* pos, Vec3s* rot) {
    EnHorse* bike;
    f32 tuck;

    // Getting on or off, Link is attached to the bike (rideActor) but not yet / no longer riding it: Epona's
    // animations play as they are (their vertical vault is scaled down where Player applies it, see z_player.c).
    if (player->rideActor != NULL && player->rideActor->id == ACTOR_EN_HORSE &&
        ((EnHorse*)player->rideActor)->type == HORSE_MOTO && Rider_IsMountingOrDismounting(player)) {
        return;
    }

    if (!MotoLink_PlayerOnBike(play)) {
        return;
    }
    bike = (EnHorse*)player->rideActor;

    // Tuck grows with speed (for tuning, gMotoLink.Pose.TuckPin 1 pins it at gMotoLink.Pose.TuckForce percent).
    // Console values stay non-negative: the console's "set" stores a negative number as a string, which crashes
    // when the variable already holds an integer (libultraship ConsoleVariable::SetString).
    tuck = CVarGetInteger("gMotoLink.Pose.TuckPin", 0)
               ? CVarGetInteger("gMotoLink.Pose.TuckForce", 0) / 100.0f
               : CLAMP(bike->actor.speedXZ / 24.0f, 0.0f, 1.0f) * CVarGetFloat("gMotoLink.Pose.Tuck", 1.0f);

    if (limbIndex == PLAYER_LIMB_ROOT) {
        pos->x = 0.0f;
        pos->y = RIDER_SEATED_PELVIS_Y;
        pos->z = 0.0f;
        Rider_ApplyBlended(limbIndex, tuck, rot);
        // Link's origin sits on the ground line under the seat, so tilting here leans him with the bike.
        EnHorseMoto_ApplyRiderTilt(bike, player);
        return;
    }
    if (bike->actor.speedXZ < RIDER_FOOT_DOWN_SPEED && CVarGetInteger("gMotoLink.Pose.FootDown", 1) &&
        Rider_Apply("gMotoLink.FootDown", sFootDown, ARRAY_COUNT(sFootDown), limbIndex, rot)) {
        return;
    }
    Rider_ApplyBlended(limbIndex, tuck, rot);
}

// The seated or tucked pose's current value for one joint axis (console override or default); the test driver's
// pose fitter starts from these.
s16 MotoLink_RiderPoseValue(u8 limb, char axis, bool tuck) {
    return Rider_PoseValue(tuck, limb, axis);
}

Vec3f gMotoRiderFootAxes[2][4];

void MotoLink_RiderPostLimb(Player* player, s32 limbIndex) {
    static const Vec3f sAxes[4] = { { 0.0f, 0.0f, 0.0f }, { 1000.0f, 0.0f, 0.0f }, { 0.0f, 1000.0f, 0.0f },
                                    { 0.0f, 0.0f, 1000.0f } };
    s32 foot = (limbIndex == PLAYER_LIMB_L_FOOT) ? 0 : (limbIndex == PLAYER_LIMB_R_FOOT) ? 1 : -1;
    s32 i;

    if (foot < 0) {
        return;
    }
    for (i = 0; i < 4; i++) {
        Matrix_MultVec3f(&sAxes[i], &gMotoRiderFootAxes[foot][i]);
    }
}
