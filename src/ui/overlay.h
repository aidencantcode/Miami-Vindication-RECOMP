#pragma once
// In-game overlay drawn on top of the game image (Dear ImGui, OpenGL 3 backend):
//   - the settings window (F1, or the controller's Guide button / L3+R3), which pauses the game
//   - an FPS counter (settings: show_fps)
//   - short toast messages ("Controller connected", ...)
// Drawn into the window after gles1::vfb_present; ImGui's GL3 backend saves and restores the GL
// state it touches, and never touches the fixed-function state the game relies on.
#include <SDL.h>

#include <string>

namespace settings { struct Settings; }

namespace ui {

struct FrameStats {
    double fps = 0;
    double game_ms = 0;        // time inside nativeRender per frame
    int render_w = 0, render_h = 0;  // internal resolution
    int game_w = 0, game_h = 0;      // what the game is told
    int fast_forward = 0;            // current fast-forward speed (0 = off)
};

bool init(SDL_Window* win, SDL_GLContext ctx);
void shutdown();

// Returns true if the overlay used the event, so the game must not see it.
bool handle_event(const SDL_Event& ev);
bool is_open();
void set_open(bool open);

// Draws the overlay for this frame. Edits `cfg` in place; returns true if a setting changed
// (the caller applies what can change live and saves the file).
bool draw(settings::Settings& cfg, const FrameStats& st);

void toast(const std::string& msg);

}  // namespace ui
