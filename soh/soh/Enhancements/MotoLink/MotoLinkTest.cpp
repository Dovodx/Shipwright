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
#include "variables.h"
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
