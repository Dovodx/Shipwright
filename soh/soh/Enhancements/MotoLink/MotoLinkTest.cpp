// MotoLink test driver: scripted input for automated, headless playtests (run under Xvfb).
//
// Inactive unless the environment variable MOTOLINK_TEST_SCRIPT names a script file. Each line is
// "<frame> <command> [args...]", where <frame> counts game frames since the first PlayState frame
// ("+N" = N frames after the previous line; "#" starts a comment):
//
//   hold <buttons> <frames>   hold buttons (comma-separated: A,B,Z,R,L,START,CU,CD,CL,CR,DU,DD,DL,DR)
//   stick <x> <y> <frames>    hold the control stick at x,y (-80..80)
//   console <command...>      run a debug console command (e.g. "console moto")
//   shot <path>               capture the X display ($DISPLAY) to <path> with ImageMagick
//   log <text...>             print a marker line to stdout
//   actors [id]               print live actors (optionally only one actor id): id, pos, yaw, params
//   teleport <x> <y> <z> [yaw] move Link (handles negative coordinates, unlike the console's pos)
//   bike <x> <y> <z> [yaw]    move the bike in the scene (Link comes along if riding)
//   spawnactor <id> <params> <x> <y> <z> [yaw]  spawn any actor (accepts negative coordinates)
//   ingorace                  act as if Ingo's race was just accepted (what En_In does on "yes"): race state 2,
//                             reload Lon Lon Ranch with the race-start cutscene index
//   sidecam <deg> <dist> <height> | off   view Link from a fixed angle relative to his facing (0 = front,
//                             90 = his left, 270 = his right), e.g. "sidecam 90 120 30" for a side profile;
//                             follows him every frame
//   joints                    print Link's current joint rotations (skelAnime jointTable, per limb)
//   age <0|1>                 set Link's age (0 adult, 1 child) in the save context
//   eponasong                 act as if Epona's Song was just played (calls Epona, runs song hooks)
//   eventflag <hex>           set an eventChkInf flag (e.g. 0x69 raises Lake Hylia's water); reload to apply
//   sceneinit                 run the OnSceneInit hooks for the current scene (exercises age-dependent setup)
//   limbs                     print Link's body part (joint) positions in the ridden bike's space
//   mount [epona]             place Link beside the bike (or Epona), ready to get on with A
//   fitrider <stage>          fit the riding pose (stages: torso, limbs, feet, tucktorso, tuckarms) so the head sits over
//                             the tank, ankles reach the pegs and wrists the grips
//                             (pattern search on the gMotoLink.Pose.* CVars, measured in game; the script pauses
//                             until it finishes, then prints the fitted values)
//   probe                     print Link's position and, if riding, the mount's speed/yaw/action
//   quit                      exit the game
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#ifdef _WIN32
#include <process.h> // _exit
#else
#include <unistd.h>
#endif
#include <fstream>
#include <sstream>
#include <string>
#include <functional>
#include <vector>

#include <libultraship/libultraship.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"
#include "MotoLink.h"
#include "soh/Enhancements/SwitchAge.h"
#include "soh/ResourceManagerHelpers.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Horse/z_en_horse.h"
extern PlayState* gPlayState;
s16 MotoLink_RiderPoseValue(u8 limb, char axis, s32 set);
}

namespace {

struct TestEvent {
    int frame;
    std::vector<std::string> args;
};

struct HeldInput {
    u16 buttons = 0;
    s8 stickX = 0;
    s8 stickY = 0;
    int buttonsUntil = -1;
    int stickUntil = -1;
};

std::vector<TestEvent> sEvents;
size_t sNextEvent = 0;
int sFrame = -1; // -1 until the first PlayState frame
HeldInput sHeld;

struct SideCam {
    bool active = false;
    s16 camId = -1;
    f32 angleDeg = 90.0f, dist = 120.0f, height = 30.0f;
} sSideCam;

void UpdateSideCam() {
    if (!sSideCam.active || gPlayState == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    s16 yaw = player->actor.shape.rot.y + (s16)(sSideCam.angleDeg * (0x8000 / 180.0f));
    Vec3f at = player->actor.world.pos;
    at.y += 30.0f;
    Vec3f eye = { at.x + Math_SinS(yaw) * sSideCam.dist, at.y + sSideCam.height - 30.0f,
                  at.z + Math_CosS(yaw) * sSideCam.dist };
    Play_CameraSetAtEye(gPlayState, sSideCam.camId, &at, &eye);
}

u16 ParseButtons(const std::string& list) {
    static const std::pair<const char*, u16> kNames[] = {
        { "A", BTN_A },         { "B", BTN_B },         { "Z", BTN_Z },         { "R", BTN_R },
        { "L", BTN_L },         { "START", BTN_START }, { "CU", BTN_CUP },      { "CD", BTN_CDOWN },
        { "CL", BTN_CLEFT },    { "CR", BTN_CRIGHT },   { "DU", BTN_DUP },      { "DD", BTN_DDOWN },
        { "DL", BTN_DLEFT },    { "DR", BTN_DRIGHT },
    };
    u16 mask = 0;
    std::stringstream ss(list);
    std::string name;
    while (std::getline(ss, name, ',')) {
        for (const auto& [n, bit] : kNames) {
            if (name == n) {
                mask |= bit;
            }
        }
    }
    return mask;
}

void LoadScript(const char* path) {
    std::ifstream in(path);
    std::string line;
    int last = 0;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        std::stringstream ss(line);
        std::string frameStr;
        if (!(ss >> frameStr)) {
            continue;
        }
        int frame = frameStr[0] == '+' ? last + std::stoi(frameStr.substr(1)) : std::stoi(frameStr);
        last = frame;
        TestEvent ev{ frame, {} };
        std::string arg;
        while (ss >> arg) {
            ev.args.push_back(arg);
        }
        if (!ev.args.empty()) {
            sEvents.push_back(ev);
        }
    }
    printf("motolink-test: loaded %zu events from %s\n", sEvents.size(), path);
    fflush(stdout);
}

std::string JoinFrom(const std::vector<std::string>& args, size_t start) {
    std::string out;
    for (size_t i = start; i < args.size(); i++) {
        out += (i > start ? " " : "") + args[i];
    }
    return out;
}

void FitStart(const std::string& stage);
Vec3f ToBikeSpace(Actor* bike, Vec3f p);

void RunEvent(const TestEvent& ev) {
    const auto& a = ev.args;
    const std::string& cmd = a[0];
    if (cmd == "hold" && a.size() >= 3) {
        sHeld.buttons = ParseButtons(a[1]);
        sHeld.buttonsUntil = sFrame + std::stoi(a[2]);
    } else if (cmd == "stick" && a.size() >= 4) {
        sHeld.stickX = (s8)std::stoi(a[1]);
        sHeld.stickY = (s8)std::stoi(a[2]);
        sHeld.stickUntil = sFrame + std::stoi(a[3]);
    } else if (cmd == "console") {
        std::string output;
        Ship::Context::GetRawInstance()->GetConsole()->Run(JoinFrom(a, 1), &output);
        printf("motolink-test: @%d console '%s' -> %s\n", sFrame, JoinFrom(a, 1).c_str(), output.c_str());
    } else if (cmd == "shot" && a.size() >= 2) {
        std::string command = "import -silent -window root '" + a[1] + "'";
        int rc = system(command.c_str());
        printf("motolink-test: @%d shot %s rc=%d\n", sFrame, a[1].c_str(), rc);
    } else if (cmd == "probe" && gPlayState != nullptr) {
        Player* player = GET_PLAYER(gPlayState);
        Actor* ride = player->rideActor;
        printf("motolink-test: @%d probe link=(%.0f,%.0f,%.0f) linkYaw=0x%04X scene=0x%02X riding=%d age=%d "
               "childTradeSlot=0x%02X",
               sFrame, player->actor.world.pos.x, player->actor.world.pos.y, player->actor.world.pos.z,
               (u16)player->actor.shape.rot.y, gPlayState->sceneNum, ride != nullptr,
               gSaveContext.linkAge, gSaveContext.inventory.items[SLOT_TRADE_CHILD]);
        printf(" rupees=%d health=%d", gSaveContext.rupees, gSaveContext.health);
        printf(" water=%.0f sf1=0x%08X sf2=0x%08X uwTimer=%d timerState=%d timer=%d boots=%d", player->actor.yDistToWater,
               player->stateFlags1, player->stateFlags2, player->underwaterTimer, gSaveContext.timerState,
               gSaveContext.timerSeconds, player->currentBoots);
        if (ride != nullptr) {
            EnHorse* horse = (EnHorse*)ride;
            printf(" speed=%.2f yaw=0x%04X action=%d flags=0x%08X", ride->speedXZ, (u16)ride->world.rot.y,
                   horse->action, horse->stateFlags);
        }
        for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head; a != nullptr; a = a->next) {
            if (a->id == ACTOR_EN_HORSE) {
                EnHorse* h = (EnHorse*)a;
                printf(" horse[type=%d pos=(%.0f,%.0f,%.0f) floor=%.0f uw=%d v=%.1f lean=%d wheelie=%d fallen=%d wall=%d "
                       "bg=0x%X]",
                       h->type, a->world.pos.x, a->world.pos.y, a->world.pos.z, a->floorHeight, h->motoUnderwater,
                       a->speedXZ, h->motoLean, h->motoWheelie, h->motoFallen, h->motoWallTouch, a->bgCheckFlags);
            }
        }
        printf("\n");
    } else if (cmd == "actors" && gPlayState != nullptr) {
        int filter = a.size() > 1 ? std::stoi(a[1], nullptr, 0) : -1;
        printf("motolink-test: @%d actors scene=%d room=%d\n", sFrame, gPlayState->sceneNum,
               gPlayState->roomCtx.curRoom.num);
        for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
            for (Actor* act = gPlayState->actorCtx.actorLists[cat].head; act != nullptr; act = act->next) {
                if (filter < 0 || act->id == filter) {
                    printf("motolink-test:   actor id=0x%04X cat=%d pos=(%.0f,%.0f,%.0f) yaw=0x%04X params=0x%04X "
                           "health=%d\n",
                           (u16)act->id, cat, act->world.pos.x, act->world.pos.y, act->world.pos.z,
                           (u16)act->shape.rot.y, (u16)act->params, act->colChkInfo.health);
                }
            }
        }
    } else if (cmd == "teleport" && a.size() >= 4 && gPlayState != nullptr) {
        Player* player = GET_PLAYER(gPlayState);
        player->actor.world.pos = { std::stof(a[1]), std::stof(a[2]), std::stof(a[3]) };
        player->actor.prevPos = player->actor.home.pos = player->actor.world.pos;
        if (a.size() >= 5) {
            player->actor.shape.rot.y = player->actor.world.rot.y = player->yaw = (s16)std::stoi(a[4], nullptr, 0);
        }
    } else if (cmd == "eponasong" && gPlayState != nullptr) {
        gPlayState->msgCtx.lastPlayedSong = OCARINA_SONG_EPONAS;
        DREG(53) = 1; // what z_message does for Epona's Song
        GameInteractor::Instance->ExecuteHooks<GameInteractor::OnOcarinaSongAction>();
    } else if (cmd == "eventflag" && a.size() > 1) {
        Flags_SetEventChkInf(std::stoi(a[1], nullptr, 0));
    } else if (cmd == "sceneinit" && gPlayState != nullptr) {
        GameInteractor::Instance->ExecuteHooks<GameInteractor::OnSceneInit>(gPlayState->sceneNum);
    } else if (cmd == "bike" && a.size() >= 4 && gPlayState != nullptr) {
        for (Actor* act = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head; act != nullptr; act = act->next) {
            if (act->id == ACTOR_EN_HORSE &&
                ((EnHorse*)act)->type == (a.size() > 1 && a[1] == "epona" ? HORSE_EPONA : HORSE_MOTO)) {
                act->world.pos = { std::stof(a[1]), std::stof(a[2]), std::stof(a[3]) };
                act->prevPos = act->world.pos;
                if (a.size() >= 5) {
                    act->world.rot.y = act->shape.rot.y = (s16)std::stoi(a[4], nullptr, 0);
                }
            }
        }
    } else if (cmd == "spawnactor" && a.size() >= 6 && gPlayState != nullptr) {
        Actor* spawned = Actor_Spawn(&gPlayState->actorCtx, gPlayState, (s16)std::stoi(a[1], nullptr, 0), std::stof(a[3]),
                                     std::stof(a[4]), std::stof(a[5]), 0,
                                     a.size() >= 7 ? (s16)std::stoi(a[6], nullptr, 0) : 0, 0,
                                     (s16)std::stoi(a[2], nullptr, 0));
        printf("motolink-test: @%d spawnactor %s -> %s\n", sFrame, a[1].c_str(), spawned ? "ok" : "failed");
    } else if (cmd == "ingorace" && gPlayState != nullptr) {
        gSaveContext.eventInf[0] = (gSaveContext.eventInf[0] & ~0x1F) | 2;
        MotoLink_SetRacingOnBike(MotoLink_PlayerOnBike(gPlayState));
        gSaveContext.eventInf[0] |= 0x8000;
        gPlayState->nextEntranceIndex = ENTR_LON_LON_RANCH_ENTRANCE;
        gSaveContext.nextCutsceneIndex = 0xFFF0;
        gPlayState->transitionType = TRANS_TYPE_FADE_BLACK;
        gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    } else if (cmd == "sidecam" && gPlayState != nullptr) {
        if (a.size() > 1 && a[1] == "off") {
            if (sSideCam.active) {
                Play_ClearCamera(gPlayState, sSideCam.camId);
                Play_ChangeCameraStatus(gPlayState, CAM_ID_MAIN, CAM_STAT_ACTIVE);
            }
            sSideCam.active = false;
        } else {
            if (!sSideCam.active) {
                sSideCam.camId = Play_CreateSubCamera(gPlayState);
                Play_ChangeCameraStatus(gPlayState, CAM_ID_MAIN, CAM_STAT_WAIT);
                Play_ChangeCameraStatus(gPlayState, sSideCam.camId, CAM_STAT_ACTIVE);
                sSideCam.active = true;
            }
            if (a.size() >= 4) {
                sSideCam.angleDeg = std::stof(a[1]);
                sSideCam.dist = std::stof(a[2]);
                sSideCam.height = std::stof(a[3]);
            }
            UpdateSideCam();
        }
    } else if (cmd == "joints" && gPlayState != nullptr) {
        Player* player = GET_PLAYER(gPlayState);
        printf("motolink-test: @%d joints", sFrame);
        for (int i = 0; i < PLAYER_LIMB_MAX; i++) {
            Vec3s* j = &player->skelAnime.jointTable[i];
            printf(" [%d:%d,%d,%d]", i, j->x, j->y, j->z);
        }
        printf("\n");
    } else if (cmd == "floorat" && a.size() >= 3 && gPlayState != nullptr) {
        // Floor heights straight down from y=2000 at x z, then along a direction: floorat <x> <z> [yaw step n]
        f32 x = std::stof(a[1]), z = std::stof(a[2]);
        s16 yaw = a.size() > 3 ? (s16)std::stoi(a[3], nullptr, 0) : 0;
        f32 step = a.size() > 4 ? std::stof(a[4]) : 0.0f;
        int n = a.size() > 5 ? std::stoi(a[5]) : 1;
        for (int i = 0; i < n; i++) {
            Vec3f p = { x + Math_SinS(yaw) * step * i, 2000.0f, z + Math_CosS(yaw) * step * i };
            CollisionPoly* poly;
            s32 bgId;
            f32 y = BgCheck_EntityRaycastFloor3(&gPlayState->colCtx, &poly, &bgId, &p);
            printf("motolink-test: floorat (%.0f,%.0f) y=%.0f\n", p.x, p.z, y);
        }
    } else if (cmd == "fileslot" && a.size() > 1) {
        // The test boot's debug save has no file slot; Anchor only treats a save in slot 0-2 as loaded
        gSaveContext.fileNum = std::stoi(a[1]);
    } else if (cmd == "textid" && gPlayState != nullptr) {
        printf("motolink-test: @%d textid 0x%04X msgMode=%d\n", sFrame, gPlayState->msgCtx.textId,
               gPlayState->msgCtx.msgMode);
    } else if (cmd == "eventget" && a.size() > 1) {
        printf("motolink-test: @%d eventget %s = %d\n", sFrame, a[1].c_str(),
               Flags_GetEventChkInf(std::stoi(a[1], nullptr, 0)) ? 1 : 0);
    } else if (cmd == "switchage" && gPlayState != nullptr) {
        SwitchAge(); // SoH's age switch: the age flips on the next scene load, as with the Temple of Time
    } else if (cmd == "cbuttons" && gPlayState != nullptr) {
        printf("motolink-test: @%d cbuttons age=%d L=0x%02X D=0x%02X R=0x%02X tradeChild=0x%02X\n", sFrame,
               gSaveContext.linkAge, gSaveContext.equips.buttonItems[1], gSaveContext.equips.buttonItems[2],
               gSaveContext.equips.buttonItems[3], gSaveContext.inventory.items[SLOT_TRADE_CHILD]);
    } else if (cmd == "daytime" && a.size() > 1) {
        gSaveContext.dayTime = (u16)std::stoi(a[1], nullptr, 0); // e.g. 0x8000 = noon (no Stalchildren)
    } else if (cmd == "fencefind" && gPlayState != nullptr) {
        // List jumpable-looking fences in the scene's static collision: wall polygons 19..112 above the ground in
        // front of them, with the ground height behind them (a big negative drop = cliff behind the fence).
        CollisionContext* colCtx = &gPlayState->colCtx;
        CollisionHeader* header = colCtx->colHeader;
        std::vector<std::pair<int, int>> seen;
        int found = 0;
        for (int i = 0; i < header->numPolygons && found < 150; i++) {
            CollisionPoly* poly = &header->polyList[i];
            f32 nx = COLPOLY_GET_NORMAL(poly->normal.x), ny = COLPOLY_GET_NORMAL(poly->normal.y),
                nz = COLPOLY_GET_NORMAL(poly->normal.z);
            if (fabsf(ny) > 0.2f) {
                continue;
            }
            Vec3f v[3];
            CollisionPoly_GetVerticesByBgId(poly, BGCHECK_SCENE, colCtx, v);
            f32 top = std::max(v[0].y, std::max(v[1].y, v[2].y));
            Vec3f c = { (v[0].x + v[1].x + v[2].x) / 3, top + 60.0f, (v[0].z + v[1].z + v[2].z) / 3 };
            CollisionPoly* floorPoly;
            s32 bgId;
            Vec3f front = { c.x + nx * 40.0f, c.y, c.z + nz * 40.0f };
            Vec3f behind = { c.x - nx * 120.0f, c.y, c.z - nz * 120.0f };
            f32 frontY = BgCheck_EntityRaycastFloor3(colCtx, &floorPoly, &bgId, &front);
            f32 behindY = BgCheck_EntityRaycastFloor3(colCtx, &floorPoly, &bgId, &behind);
            bool blocked = behindY > BGCHECK_Y_MIN && SurfaceType_IsHorseBlocked(colCtx, floorPoly, bgId);
            f32 h = top - frontY;
            if (frontY <= BGCHECK_Y_MIN || h <= 19.0f || h > 112.0f) {
                continue;
            }
            std::pair<int, int> key = { (int)floorf(c.x / 200.0f), (int)floorf(c.z / 200.0f) };
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                continue;
            }
            seen.push_back(key);
            found++;
            printf("motolink-test: fence at (%.0f,%.0f,%.0f) approachYaw=0x%04X height=%.0f behindDrop=%.0f blocked=%d\n",
                   front.x, frontY, front.z, (u16)Math_Atan2S(-nz, -nx), h,
                   behindY > BGCHECK_Y_MIN ? behindY - frontY : -9999.0f, blocked);
        }
    } else if (cmd == "fitrider" && gPlayState != nullptr) {
        if (GET_PLAYER(gPlayState)->rideActor == nullptr) {
            printf("motolink-test: @%d fitrider: not riding, skipped\n", sFrame);
        } else {
            FitStart(a.size() > 1 ? a[1] : "limbs");
        }
    } else if (cmd == "mount" && gPlayState != nullptr) {
        // Stand Link beside the bike (its right side), facing it, ready for an A press to get on,
        // wherever the summon put the bike.
        Player* player = GET_PLAYER(gPlayState);
        for (Actor* act = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head; act != nullptr; act = act->next) {
            if (act->id == ACTOR_EN_HORSE &&
                ((EnHorse*)act)->type == (a.size() > 1 && a[1] == "epona" ? HORSE_EPONA : HORSE_MOTO)) {
                s16 yaw = act->shape.rot.y;
                player->actor.world.pos = { act->world.pos.x - Math_CosS(yaw) * 35.0f, act->world.pos.y,
                                            act->world.pos.z + Math_SinS(yaw) * 35.0f };
                player->actor.prevPos = player->actor.world.pos;
                player->actor.shape.rot.y = player->actor.world.rot.y = player->yaw =
                    Math_Vec3f_Yaw(&player->actor.world.pos, &act->world.pos); // mounting needs him facing the bike
                break;
            }
        }
    } else if (cmd == "limbs" && gPlayState != nullptr) {
        // Body part (joint) positions in the ridden bike's space: +X left, +Y up, +Z forward, origin on the ground
        // between the wheels. Compare with assets/bike/bike_spec.json (seat, pegs, grips).
        Player* player = GET_PLAYER(gPlayState);
        Actor* bike = player->rideActor;
        static const char* kNames[] = { "waist", "rThigh", "rShin", "rFoot", "lThigh", "lShin", "lFoot", "head",
                                        "hat", "collar", "lShoulder", "lForearm", "lHand", "rShoulder",
                                        "rForearm", "rHand", "sheath", "torso" };
        if (bike == nullptr) {
            bike = &player->actor; // not riding: Link's own space
        }
        if (bike != nullptr) {
            f32 sn = Math_SinS(bike->shape.rot.y), cs = Math_CosS(bike->shape.rot.y);
            void* anim = player->skelAnime.animation;
            printf("motolink-test: @%d limbs anim=%s rootY=%d", sFrame,
                   (anim != nullptr && ResourceMgr_OTRSigCheck((char*)anim)) ? (const char*)anim : "?",
                   player->skelAnime.jointTable[0].y);
            for (int i = 0; i < PLAYER_BODYPART_MAX; i++) {
                Vec3f d = { player->bodyPartsPos[i].x - bike->world.pos.x, player->bodyPartsPos[i].y - bike->world.pos.y,
                            player->bodyPartsPos[i].z - bike->world.pos.z };
                printf(" %s=(%.1f,%.1f,%.1f)", kNames[i], d.x * cs - d.z * sn, d.y, d.x * sn + d.z * cs);
            }
            // Foot axes: direction of the foot limb's local X/Y/Z in bike space (unit vectors), per foot.
            for (int f = 0; f < 2; f++) {
                Vec3f p0 = ToBikeSpace(bike, gMotoRiderFootAxes[f][0]);
                printf(" %sFootAxes=", f == 0 ? "l" : "r");
                for (int k = 1; k < 4; k++) {
                    Vec3f pk = ToBikeSpace(bike, gMotoRiderFootAxes[f][k]);
                    printf("%c(%.2f,%.2f,%.2f)", "xyz"[k - 1], (pk.x - p0.x) / 10.0f, (pk.y - p0.y) / 10.0f,
                           (pk.z - p0.z) / 10.0f);
                }
            }
            printf("\n");
        }
    } else if (cmd == "age" && a.size() > 1) {
        gSaveContext.linkAge = std::stoi(a[1]);
    } else if (cmd == "log") {
        printf("motolink-test: @%d %s\n", sFrame, JoinFrom(a, 1).c_str());
    } else if (cmd == "quit") {
        printf("motolink-test: @%d quit\n", sFrame);
        fflush(stdout);
        _exit(0); // skip shutdown destructors, which can hang without a real display/audio device
    }
    fflush(stdout);
}

// Body part positions in the ridden bike's space (+X left, +Y up, +Z forward, origin on the ground between the
// wheels), undoing the bike's yaw (fit with the lean at 0).
Vec3f ToBikeSpace(Actor* bike, Vec3f p) {
    f32 sn = Math_SinS(bike->shape.rot.y), cs = Math_CosS(bike->shape.rot.y);
    Vec3f d = { p.x - bike->world.pos.x, p.y - bike->world.pos.y, p.z - bike->world.pos.z };
    return { d.x * cs - d.z * sn, d.y, d.x * sn + d.z * cs };
}

bool BikeSpacePart(int part, Vec3f* out) {
    Player* player = GET_PLAYER(gPlayState);
    Actor* bike = player->rideActor;
    if (bike == nullptr) {
        return false;
    }
    f32 sn = Math_SinS(bike->shape.rot.y), cs = Math_CosS(bike->shape.rot.y);
    Vec3f d = { player->bodyPartsPos[part].x - bike->world.pos.x, player->bodyPartsPos[part].y - bike->world.pos.y,
                player->bodyPartsPos[part].z - bike->world.pos.z };
    *out = { d.x * cs - d.z * sn, d.y, d.x * sn + d.z * cs };
    return true;
}

// Rider pose fitter. Each chain (torso, a leg or an arm) is fitted by a pattern search over its joint angles: one
// angle at a time is nudged by +-step, kept if the chain's score improved, and the step halves after a full pass
// without improvement. Every trial is measured on the drawn model two frames later. Chains in one stage are
// independent (separate branches of the skeleton), so they are searched in parallel.
struct FitParam {
    int limb;
    char axis;
};
struct FitChain {
    const char* name;
    std::vector<FitParam> params;
    std::function<f32()> score;
    // search state
    std::vector<s16> values;
    size_t param = 0;
    int dir = 1;
    int step = 8192; // big first steps: the arms start far from the grips
    f32 best = -1.0f;
    bool improved = false;
    bool done = false;
};

const char* sFitPrefix = "gMotoLink.Pose";

std::string FitCVar(const FitParam& p) {
    return std::string(sFitPrefix) + "." + std::to_string(p.limb) + "." + std::string(1, p.axis);
}

Vec3f Part(int part) {
    Vec3f v = { 0.0f, 0.0f, 0.0f };
    BikeSpacePart(part, &v);
    return v;
}

f32 DistSq(Vec3f a, Vec3f b) {
    return SQ(a.x - b.x) + SQ(a.y - b.y) + SQ(a.z - b.z);
}

// End joint (ankle, wrist) on its target, middle joint (knee, elbow) softly near where it should be.
std::function<f32()> ReachScore(int endPart, Vec3f end, int midPart, Vec3f mid, f32 midWeight) {
    return [=]() { return DistSq(Part(endPart), end) + midWeight * DistSq(Part(midPart), mid); };
}

// Torso: head where wanted, shoulders level and centred.
std::function<f32()> TorsoScore(Vec3f head) {
    return [=]() {
        Vec3f l = Part(PLAYER_BODYPART_L_SHOULDER), r = Part(PLAYER_BODYPART_R_SHOULDER);
        return DistSq(Part(PLAYER_BODYPART_HEAD), head) + 2.0f * (SQ(l.y - r.y) + SQ(l.x + r.x) + SQ(l.z - r.z));
    };
}

// Foot on the peg: the foot limb's +X (heel to toe) points forward, a little down and out; its +Y (through the
// sole, downwards) points down. foot: 0 left, 1 right.
std::function<f32()> FootScore(int foot) {
    return [=]() {
        Player* player = GET_PLAYER(gPlayState);
        Actor* bike = player->rideActor;
        if (bike == nullptr) {
            return 0.0f;
        }
        f32 side = (foot == 0) ? 1.0f : -1.0f;
        Vec3f toe = { 0.2f * side, -0.2f, 0.96f }; // ~12 degrees out, ~12 degrees down
        Vec3f sole = { 0.0f, -0.98f, -0.2f };      // perpendicular to toe (down, tilted back with it)
        Vec3f p0 = ToBikeSpace(bike, gMotoRiderFootAxes[foot][0]);
        Vec3f px = ToBikeSpace(bike, gMotoRiderFootAxes[foot][1]);
        Vec3f py = ToBikeSpace(bike, gMotoRiderFootAxes[foot][2]);
        Vec3f ax = { (px.x - p0.x) / 10.0f, (px.y - p0.y) / 10.0f, (px.z - p0.z) / 10.0f };
        Vec3f ay = { (py.x - p0.x) / 10.0f, (py.y - p0.y) / 10.0f, (py.z - p0.z) / 10.0f };
        return 100.0f * (DistSq(ax, toe) + DistSq(ay, sole));
    };
}

void FitPropose(FitChain& c) {
    s16 v = c.values[c.param] + c.dir * c.step;
    CVarSetInteger(FitCVar(c.params[c.param]).c_str(), v);
}

void FitRevert(FitChain& c) {
    CVarSetInteger(FitCVar(c.params[c.param]).c_str(), c.values[c.param]);
}

std::vector<FitChain> sFitChains;
int sFitNextTick = -1;
int sFitTicks = 0;

// Stages: "torso" / "limbs" fit the seated pose; "tucktorso" / "tuckarms" the full-speed tuck (pinned with
// gMotoLink.Pose.TuckForce 100 meanwhile). Targets from assets/bike/bike_spec.json: pegs (+-6, 12.4, -9.5), grips
// (+-7.4, 34.1, 10.6), seat (0, 27, -7). The ankle sits above and behind the peg (ball of the foot on it); the
// wrist a little behind the grip (palm around it).
void FitStart(const std::string& stage) {
    const Vec3f wristL = { 7.0f, 34.5f, 8.0f }, wristR = { -7.0f, 34.5f, 8.0f };
    const Vec3f elbowL = { 10.0f, 38.0f, 1.0f }, elbowR = { -10.0f, 38.0f, 1.0f };
    auto arms = [&]() -> std::vector<FitChain> {
        return {
            { "rArm",
              { { PLAYER_LIMB_R_SHOULDER, 'x' }, { PLAYER_LIMB_R_SHOULDER, 'y' }, { PLAYER_LIMB_R_SHOULDER, 'z' },
                { PLAYER_LIMB_R_FOREARM, 'x' }, { PLAYER_LIMB_R_FOREARM, 'y' }, { PLAYER_LIMB_R_FOREARM, 'z' } },
              ReachScore(PLAYER_BODYPART_R_HAND, wristR, PLAYER_BODYPART_R_FOREARM, elbowR, 0.1f) },
            { "lArm",
              { { PLAYER_LIMB_L_SHOULDER, 'x' }, { PLAYER_LIMB_L_SHOULDER, 'y' }, { PLAYER_LIMB_L_SHOULDER, 'z' },
                { PLAYER_LIMB_L_FOREARM, 'x' }, { PLAYER_LIMB_L_FOREARM, 'y' }, { PLAYER_LIMB_L_FOREARM, 'z' } },
              ReachScore(PLAYER_BODYPART_L_HAND, wristL, PLAYER_BODYPART_L_FOREARM, elbowL, 0.1f) },
        };
    };
    const std::vector<FitParam> torso = { { PLAYER_LIMB_UPPER, 'x' }, { PLAYER_LIMB_UPPER, 'y' }, { PLAYER_LIMB_UPPER, 'z' } };
    bool tuck = stage.rfind("tuck", 0) == 0;
    bool footDown = stage.rfind("footdown", 0) == 0;

    sFitPrefix = tuck ? "gMotoLink.Tuck" : footDown ? "gMotoLink.FootDown" : "gMotoLink.Pose";
    if (stage == "torso") {
        sFitChains = { { "torso", torso, TorsoScore({ 0.0f, 48.0f, -2.0f }) } };
    } else if (stage == "tucktorso") {
        sFitChains = { { "torso", torso, TorsoScore({ 0.0f, 44.0f, 3.0f }) } };
    } else if (stage == "feet") {
        sFitChains = {
            { "lFoot", { { PLAYER_LIMB_L_FOOT, 'x' }, { PLAYER_LIMB_L_FOOT, 'y' }, { PLAYER_LIMB_L_FOOT, 'z' } },
              FootScore(0) },
            { "rFoot", { { PLAYER_LIMB_R_FOOT, 'x' }, { PLAYER_LIMB_R_FOOT, 'y' }, { PLAYER_LIMB_R_FOOT, 'z' } },
              FootScore(1) },
        };
    } else if (stage == "footdown") {
        // Stopped, the left leg reaches down beside the bike: ankle at standing height (5) on the ground a little out
        // from the bike, knee bent above it.
        sFitChains = { { "lLegDown",
                         { { PLAYER_LIMB_L_THIGH, 'x' }, { PLAYER_LIMB_L_THIGH, 'y' }, { PLAYER_LIMB_L_THIGH, 'z' },
                           { PLAYER_LIMB_L_SHIN, 'z' } },
                         ReachScore(PLAYER_BODYPART_L_FOOT, { 13.0f, 5.2f, -6.0f }, PLAYER_BODYPART_L_SHIN,
                                    { 11.0f, 16.0f, 0.0f }, 0.2f) } };
    } else if (stage == "footdownfoot") {
        sFitChains = { { "lFootDown", { { PLAYER_LIMB_L_FOOT, 'x' }, { PLAYER_LIMB_L_FOOT, 'y' }, { PLAYER_LIMB_L_FOOT, 'z' } },
                         FootScore(0) } };
    } else if (stage == "tuckarms") {
        sFitChains = arms();
    } else {
        sFitChains = arms();
        sFitChains.push_back({ "rLeg",
                               { { PLAYER_LIMB_R_THIGH, 'x' }, { PLAYER_LIMB_R_THIGH, 'y' }, { PLAYER_LIMB_R_THIGH, 'z' },
                                 { PLAYER_LIMB_R_SHIN, 'z' } },
                               ReachScore(PLAYER_BODYPART_R_FOOT, { -6.5f, 15.0f, -12.5f }, PLAYER_BODYPART_R_SHIN,
                                          { -9.0f, 24.0f, 1.0f }, 0.3f) });
        sFitChains.push_back({ "lLeg",
                               { { PLAYER_LIMB_L_THIGH, 'x' }, { PLAYER_LIMB_L_THIGH, 'y' }, { PLAYER_LIMB_L_THIGH, 'z' },
                                 { PLAYER_LIMB_L_SHIN, 'z' } },
                               ReachScore(PLAYER_BODYPART_L_FOOT, { 6.5f, 15.0f, -12.5f }, PLAYER_BODYPART_L_SHIN,
                                          { 9.0f, 24.0f, 1.0f }, 0.3f) });
    }
    for (auto& c : sFitChains) {
        if (tuck) {
            c.step = 2048; // stay near the seated solution, so blending between the two poses doesn't flail
        }
        for (auto& p : c.params) {
            c.values.push_back(MotoLink_RiderPoseValue(p.limb, p.axis, tuck ? 1 : footDown ? 2 : 0));
            CVarSetInteger(FitCVar(p).c_str(), c.values.back());
        }
    }
    sFitTicks = 0;
    sFitNextTick = sFrame + 2;
}

void FitReport(const char* when) {
    for (auto& c : sFitChains) {
        printf("motolink-test: @%d fit %s %s score=%.2f", sFrame, when, c.name, c.score());
        for (size_t i = 0; i < c.params.size(); i++) {
            printf(" %s=%d", FitCVar(c.params[i]).c_str(), c.values[i]);
        }
        printf("\n");
    }
}

// Returns true while fitting (the script waits).
bool FitUpdate() {
    if (sFitNextTick < 0) {
        return false;
    }
    if (sFrame < sFitNextTick) {
        return true;
    }
    sFitNextTick = sFrame + 2;
    if (sFitTicks++ == 0) {
        FitReport("start");
    }

    bool allDone = true;
    for (auto& c : sFitChains) {
        if (c.done) {
            continue;
        }
        allDone = false;
        f32 score = c.score();
        if (c.best < 0.0f) { // baseline measured: start the search
            c.best = score;
            FitPropose(c);
            continue;
        }
        if (score < c.best) {
            // Keep it, and keep moving the same way.
            c.best = score;
            c.values[c.param] += c.dir * c.step;
            c.improved = true;
            FitPropose(c);
            continue;
        }
        FitRevert(c);
        if (c.dir > 0) {
            c.dir = -1;
        } else {
            c.dir = 1;
            if (++c.param == c.params.size()) {
                c.param = 0;
                if (!c.improved) {
                    c.step /= 2;
                    if (c.step < 32) {
                        c.done = true;
                        continue;
                    }
                }
                c.improved = false;
            }
        }
        FitPropose(c);
    }
    if (allDone || sFitTicks > 4000) {
        FitReport("end");
        sFitNextTick = -1;
        return false;
    }
    return true;
}

void OnFrame() {
    if (sFrame < 0) {
        if (gPlayState == nullptr) {
            return;
        }
        sFrame = 0;
    } else {
        sFrame++;
    }
    if (FitUpdate()) {
        // The script waits for the fitter; later events keep their spacing relative to when it finishes.
        for (size_t i = sNextEvent; i < sEvents.size(); i++) {
            sEvents[i].frame++;
        }
    }
    while (sNextEvent < sEvents.size() && sEvents[sNextEvent].frame <= sFrame) {
        RunEvent(sEvents[sNextEvent++]);
    }
    UpdateSideCam();
}

void RegisterMotoLinkTest() {
    const char* path = getenv("MOTOLINK_TEST_SCRIPT");
    if (path == nullptr || !sEvents.empty()) {
        return;
    }
    LoadScript(path);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(OnFrame);
}

static RegisterShipInitFunc initMotoLinkTest(RegisterMotoLinkTest);

} // namespace

// Called by PadMgr after controller 1's state is read (see padmgr.c); applies scripted input.
extern "C" void MotoLinkTest_OverrideInput(OSContPad* pad) {
    if (sEvents.empty() || sFrame < 0) {
        return;
    }
    if (sFrame < sHeld.buttonsUntil) {
        pad->button |= sHeld.buttons;
    }
    if (sFrame < sHeld.stickUntil) {
        pad->stick_x = sHeld.stickX;
        pad->stick_y = sHeld.stickY;
    }
}
