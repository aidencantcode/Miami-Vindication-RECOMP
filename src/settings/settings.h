#pragma once
// User settings, stored as an INI file (settings.ini in the user folder). Unknown keys are
// ignored, missing keys keep their defaults, so old files keep working.
#include <filesystem>
#include <string>

namespace settings {

struct Settings {
    // [display]
    std::string window_mode = "windowed";  // windowed | borderless | fullscreen
    int window_w = 1280, window_h = 720;    // windowed size
    bool vsync = true;
    int fps_limit = -1;                     // -1 = display refresh rate, 0 = unlimited, else 30..360 (the phone ran ~30)
    std::string aspect = "auto";            // game width at 480 high: auto (window) | 4:3 | 5:3 | 16:10 | 16:9 | 21:9
    int render_scale = 0;                   // 0 = auto (>= window resolution), else 1..8 x 480p
    int msaa = 4;                           // 0 | 2 | 4 | 8
    std::string fit = "aspect";             // aspect | stretch | integer
    bool smooth_scaling = true;             // linear filter when scaling to the window
    bool show_fps = false;

    // [graphics]
    std::string quality = "high";           // phone (exactly as on the device) | low | high | ultra (more NPCs)
    bool mipmaps = true;                    // the phone profile turned them off
    float draw_distance = 1.5f;             // multiplier on the profile's far plane + texture streaming radius
    int anisotropy = 8;                     // 0/1 = off, 2..16

    // [controls]
    std::string touch_controls = "auto";    // auto (hide while a pad/keyboard drives) | show | hide
    float mouse_sensitivity = 1.0f;
    bool invert_camera_y = false;
    float stick_deadzone = 0.2f;

    // [audio]
    float master_volume = 1.0f;             // 0..1
    float music_volume = 1.0f;              // music + radio (m_*)
    float sfx_volume = 1.0f;                // effects
    float voice_volume = 1.0f;              // speech + cutscene audio (vfx_*, cin_*)
    bool restore_missing_sounds = true;     // 41 sounds the phone never found (name mix-ups): use the game's own name

    // [game]
    bool skip_intro = true;                 // skip the content warning / intro movie screens where possible
    bool pause_when_unfocused = true;
    bool hide_store_buttons = true;         // remove the dead Gameloft LIVE! / More Games! main-menu buttons
    bool custom_text = true;                // "Press any button to start", port credit in the credits
    int fast_forward_speed = 3;             // game speed while fast forward is on (F2 / R3): 2..8
};

bool load(const std::filesystem::path& file, Settings* s);   // false if the file did not exist
bool save(const std::filesystem::path& file, const Settings& s);
// Command-line override "section.key=value" (not saved unless something else saves the file).
bool apply_override(Settings* s, const std::string& assignment);

// Derived values
int logical_width(const Settings& s, int window_w, int window_h);  // game width for a 480-high screen
int auto_render_scale(int window_h);                                 // smallest scale covering the window

}  // namespace settings
