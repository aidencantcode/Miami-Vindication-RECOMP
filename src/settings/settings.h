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
    int render_scale = 0;                   // 0 = native (exactly the window), else 1..8 x 480p (supersampling)
    int msaa = 4;                           // 0 | 2 | 4 | 8
    std::string fit = "aspect";             // aspect | stretch | integer
    bool smooth_scaling = true;             // linear filter when scaling to the window
    bool show_fps = false;
    bool low_latency = true;                // wait for the GPU after each frame so frames never queue up

    // [graphics]
    std::string renderer = "gl";            // gl (desktop fixed function) | shader (the GLES 3 renderer Android uses); restart
    std::string quality = "high";           // phone (game's own pick = profile 1, untouched) | low (fastest) | high | ultra (more NPCs)
    bool mipmaps = true;                    // the phone profile turned them off
    float draw_distance = 3.0f;             // 0.5..8, live: multiplier on the detailed-city radius (where the low-detail city starts) and the far city
    int anisotropy = 8;                     // 0/1 = off, 2..16
    std::string texture_filter = "game";    // game | nearest | bilinear | trilinear
    bool night_textures = true;             // restore the unused night city textures (game/night_textures)
    float night_people_lift = 0.35f;        // people/cars at night and dusk: 0 = as dark as the city, 1 = as the game drew them
    std::string fog = "atmospheric";        // game (thin, only on the far city) | atmospheric (haze by distance, hides the horizon)
    float fog_strength = 1.5f;              // atmospheric fog: 0 = none, 1 = 97% at the far city's end
    bool bloom = true;                      // glow around bright parts (screen effect)
    float bloom_intensity = 0.8f;           // 0..2 (up to 3x more at night, see post.cpp)
    float bloom_threshold = 0.75f;          // 0..1: brightness where the glow starts
    float bloom_radius = 1.0f;              // 0.5..2: glow size
    bool height_fog = true;                 // fog step 2: thicker near the ground and the sea, sun haze
    float height_fog_amount = 1.0f;         // 0..2
    bool dynamic_lights = true;             // light pools around lamps, neon and car lights (from the game's glow sprites)
    float dynamic_lights_intensity = 1.0f;  // 0..2

    // [controls]
    std::string touch_controls = "auto";    // auto (hide while a pad/keyboard drives) | show | hide
    float mouse_sensitivity = 1.0f;
    bool invert_camera_y = false;
    bool mouse_look = true;                 // lock the cursor in gameplay; the mouse turns the camera, left click attacks
    std::string camera_mode = "freelook";   // classic (phone: swings behind you) | freelook (GTA VC PC) | first_person
    float freelook_return_delay = 1.5f;     // free look: seconds without camera input before it turns back behind you
    float freelook_return_speed = 90.0f;    // free look: how fast it turns back (degrees per second; 0 = never)
    float stick_deadzone = 0.2f;
    bool tap_to_target = false;             // the phone's tap-on-a-person lock and automatic lock while shooting (touchscreens)
    float manual_aim_spread = 0.25f;        // aiming with the crosshair: fraction of the game's random bullet spread
    std::string tilt_axes = "handheld";     // Android tilt steering: handheld (landscape-native screen) | phone (portrait-native)
    bool tilt_invert = false;               // Android tilt steering turns the wrong way: flip it

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
    bool smooth_physics = true;             // one physics step per frame (the game: fixed 60 Hz, so motion stepped at 60)
};

bool load(const std::filesystem::path& file, Settings* s);   // false if the file did not exist
bool save(const std::filesystem::path& file, const Settings& s);
// Command-line override "section.key=value" (not saved unless something else saves the file).
bool apply_override(Settings* s, const std::string& assignment);

// Derived values
int logical_width(const Settings& s, int window_w, int window_h);  // game width for a 480-high screen

}  // namespace settings
