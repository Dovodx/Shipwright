/*
 * MotoLink: Link's riding pose on the bike.
 *
 * Instead of new animation files, the pose is applied procedurally on top of whatever riding animation plays: while
 * Link is on the bike, the limbs listed below take fixed rotations (sitting with knees forward, hands on the bars)
 * in Player's limb-draw callback. Every value can be overridden at runtime with the console variable
 * gMotoLink.Pose.<limb>.<x|y|z> (e.g. "set gMotoLink.Pose.4.z -18000"), which is how the pose is tuned.
 */
#include <stdio.h>

#include "global.h"
#include "soh/Enhancements/MotoLink/MotoLink.h"
#include "overlays/actors/ovl_En_Horse/z_en_horse.h"
#include "objects/gameplay_keep/gameplay_keep.h"
#include "soh/ResourceManagerHelpers.h"

typedef struct {
    u8 limb;
    s16 x, y, z;
} RiderJoint;

// Seated pose (limb-local rotations, binary angles): thighs forward along the tank, shins down to the pegs. Arms
// keep the riding animation, which already reaches forward. Provisional until the generated bike model fixes the
// seat, peg and bar positions.
static const RiderJoint sBikePose[] = {
    { PLAYER_LIMB_R_THIGH, 2371, 4500, -11000 },
    { PLAYER_LIMB_R_SHIN, 0, 0, 18000 },
    { PLAYER_LIMB_R_FOOT, 2416, 929, -18612 },
    { PLAYER_LIMB_L_THIGH, -2104, -4500, -11000 },
    { PLAYER_LIMB_L_SHIN, 0, 0, 18000 },
    { PLAYER_LIMB_L_FOOT, -1722, -2045, -19000 },
};

// Stopped: the left leg goes down to the ground, a little out to the side (the bike tips onto it).
static const RiderJoint sFootDown[] = {
    { PLAYER_LIMB_L_THIGH, -2104, -6500, -3886 },
    { PLAYER_LIMB_L_SHIN, 0, 0, 3000 },
    { PLAYER_LIMB_L_FOOT, -1722, -2045, -19000 },
};

// Tuck: the upper body leans forward over the tank with speed (added to the riding animation's torso).
#define RIDER_TUCK_MAX 3500
#define RIDER_FOOT_DOWN_SPEED 0.5f

// Getting on/off: Epona's mount/dismount animations vault Link up to (or down from) a saddle about 40 units higher
// than the bike's seat. Scaling the animation's vertical root motion down turns the vault into a leg swing.
#define RIDER_STANDING_PELVIS_Y 3464.0f // root height of Link's standing pose (model units)
#define RIDER_MOUNT_RISE_SCALE 0.2f

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

static s16 Rider_Value(u8 limb, char axis, s16 defaultValue) {
    char name[48];

    snprintf(name, sizeof(name), "gMotoLink.Pose.%d.%c", limb, axis);
    return (s16)CVarGetInteger(name, defaultValue);
}

static bool Rider_Apply(const RiderJoint* pose, s32 count, s32 limbIndex, Vec3s* rot) {
    s32 i;

    for (i = 0; i < count; i++) {
        if (pose[i].limb == limbIndex) {
            rot->x = Rider_Value(pose[i].limb, 'x', pose[i].x);
            rot->y = Rider_Value(pose[i].limb, 'y', pose[i].y);
            rot->z = Rider_Value(pose[i].limb, 'z', pose[i].z);
            return true;
        }
    }
    return false;
}

void MotoLink_RiderLimbOverride(PlayState* play, Player* player, s32 limbIndex, Vec3f* pos, Vec3s* rot) {
    EnHorse* bike;
    s32 i;

    // Getting on or off, Link is attached to the bike (rideActor) but not yet / no longer riding it.
    if (player->rideActor != NULL && player->rideActor->id == ACTOR_EN_HORSE &&
        ((EnHorse*)player->rideActor)->type == HORSE_MOTO && Rider_IsMountingOrDismounting(player)) {
        if (limbIndex == PLAYER_LIMB_ROOT) {
            pos->y = RIDER_STANDING_PELVIS_Y +
                     (pos->y - RIDER_STANDING_PELVIS_Y) * CVarGetFloat("gMotoLink.Pose.MountRise", RIDER_MOUNT_RISE_SCALE);
        }
        return;
    }

    if (!MotoLink_PlayerOnBike(play)) {
        return;
    }
    bike = (EnHorse*)player->rideActor;

    if (limbIndex == PLAYER_LIMB_ROOT) {
        // Link's origin sits on the ground line under the seat, so rolling here leans him with the bike.
        Matrix_RotateZ(BINANG_TO_RAD(-bike->motoLean), MTXMODE_APPLY);
        return;
    }
    if (limbIndex == PLAYER_LIMB_UPPER) {
        f32 speedFrac = CLAMP(bike->actor.speedXZ / 24.0f, 0.0f, 1.0f);
        rot->z += (s16)(CVarGetInteger("gMotoLink.Pose.Tuck", RIDER_TUCK_MAX) * speedFrac);
        return;
    }
    if (bike->actor.speedXZ < RIDER_FOOT_DOWN_SPEED && Rider_Apply(sFootDown, ARRAY_COUNT(sFootDown), limbIndex, rot)) {
        return;
    }
    for (i = 0; i < ARRAY_COUNT(sBikePose); i++) {
        if (sBikePose[i].limb == limbIndex) {
            rot->x = Rider_Value(sBikePose[i].limb, 'x', sBikePose[i].x);
            rot->y = Rider_Value(sBikePose[i].limb, 'y', sBikePose[i].y);
            rot->z = Rider_Value(sBikePose[i].limb, 'z', sBikePose[i].z);
            return;
        }
    }
}
