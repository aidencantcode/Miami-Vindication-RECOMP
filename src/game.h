#pragma once
#include "rt/runtime.h"

struct RunOptions {
    int frames = 0;      // 0 = until the window is closed
    int shot_every = 0;  // save a screenshot every N frames (0 = never)
    int demo = 1;        // Gangster2.nativeInit arg; 1 = what Java passes on a fresh install
};

int run_game(const rt::Paths& paths, const RunOptions& opt);
