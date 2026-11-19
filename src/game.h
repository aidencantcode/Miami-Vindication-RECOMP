#pragma once
#include "rt/runtime.h"

#include <string>
#include <vector>

struct RunOptions {
    int frames = 0;      // 0 = until the window is closed
    int shot_every = 0;  // save a screenshot every N frames (0 = never)
    int demo = 1;        // Gangster2.nativeInit arg; 1 = what Java passes on a fresh install
    std::string input;   // scripted input, e.g. "600:tap 400 240; 900:key 4"
    std::string shots_at;  // extra screenshot frames, e.g. "650,700,900"
    int game_w = 0, game_h = 480;  // override the game's screen width (0 = from settings/window aspect)
    bool profile_traps = false;      // log the busiest import traps every 2 s
    int fps = -1;                    // override settings fps_limit (0 = unlimited)
    bool no_vsync = false;           // override settings vsync
    double seconds = 0;              // stop after this many seconds (0 = no limit)
    double profile_guest_at = -1;    // start the guest instruction profiler at this time (s)
    double dump_hud_at = -1;         // log the HUD controls once at this time (s)
    std::string touch_controls;      // override settings: show | hide | auto
    std::string quality;             // override settings: phone | low | high | ultra
    bool no_mods = false;            // run without game/mods hooks (vanilla behaviour, for A/B tests)
    double open_settings_at = -1;    // open the settings menu at this time (s), for screenshots
    std::vector<std::string> set;    // settings overrides "section.key=value" (--set, repeatable)
    std::string audio_dump;          // write the mixed audio output to this WAV file at exit
    bool fast_forward = false;       // start with fast forward on (tests)
    bool recomp = false;             // --cpu recomp: run the recompiled code instead of Unicorn
    bool no_fastpath = false;        // --no-fastpath: Unicorn without the guest VFP helpers (same traps as recomp)
};

int run_game(const rt::Paths& paths, const RunOptions& opt);
