#pragma once
#include "rt/runtime.h"

#include <string>

struct RunOptions {
    int frames = 0;      // 0 = until the window is closed
    int shot_every = 0;  // save a screenshot every N frames (0 = never)
    int demo = 1;        // Gangster2.nativeInit arg; 1 = what Java passes on a fresh install
    std::string input;   // scripted input, e.g. "600:tap 400 240; 900:key 4"
    std::string shots_at;  // extra screenshot frames, e.g. "650,700,900"
};

int run_game(const rt::Paths& paths, const RunOptions& opt);
