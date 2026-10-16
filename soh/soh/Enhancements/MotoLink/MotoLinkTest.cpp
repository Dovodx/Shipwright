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
//   age <0|1>                 set Link's age (0 adult, 1 child) in the save context
//   eponasong                 act as if Epona's Song was just played (calls Epona, runs song hooks)
//   eventflag <hex>           set an eventChkInf flag (e.g. 0x69 raises Lake Hylia's water); reload to apply
//   sceneinit                 run the OnSceneInit hooks for the current scene (exercises age-dependent setup)
//   probe                     print Link's position and, if riding, the mount's speed/yaw/action
//   quit                      exit the game
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <libultraship/libultraship.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Horse/z_en_horse.h"
extern PlayState* gPlayState;
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
        printf("motolink-test: @%d probe link=(%.0f,%.0f,%.0f) riding=%d age=%d childTradeSlot=0x%02X", sFrame,
               player->actor.world.pos.x, player->actor.world.pos.y, player->actor.world.pos.z, ride != nullptr,
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
                printf(" horse[type=%d pos=(%.0f,%.0f,%.0f) floor=%.0f uw=%d]", ((EnHorse*)a)->type, a->world.pos.x,
                       a->world.pos.y, a->world.pos.z, a->floorHeight, ((EnHorse*)a)->motoUnderwater);
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
                    printf("motolink-test:   actor id=0x%04X cat=%d pos=(%.0f,%.0f,%.0f) yaw=0x%04X params=0x%04X\n",
                           (u16)act->id, cat, act->world.pos.x, act->world.pos.y, act->world.pos.z,
                           (u16)act->shape.rot.y, (u16)act->params);
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
            if (act->id == ACTOR_EN_HORSE && ((EnHorse*)act)->type == HORSE_MOTO) {
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

void OnFrame() {
    if (sFrame < 0) {
        if (gPlayState == nullptr) {
            return;
        }
        sFrame = 0;
    } else {
        sFrame++;
    }
    while (sNextEvent < sEvents.size() && sEvents[sNextEvent].frame <= sFrame) {
        RunEvent(sEvents[sNextEvent++]);
    }
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
