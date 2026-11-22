#pragma once
// Screen effects on the 3D scene only (Phase 5C.0): the GL bridge calls run() at the end of the
// scene, just before the first HUD (orthographic) draw of the frame, or at present when nothing 2D
// follows. The scene's colour and depth are copied (MSAA resolved) into textures, the passes draw
// the result back into the game's framebuffer, and all GL state the game relies on is restored, so
// the HUD draws on top unchanged.
//
// Two stages: the atmosphere (dynamic lights, fog, the sky's horizon) runs on the solid scene and
// the sea, just before the first glow sprite is drawn, so lamp and headlight glows are not fogged
// by what is behind them; the finish (bloom, colour grade) runs at the end of the scene. Frames
// without glows run both at the end, in one pass.
//
// Shaders are written in a GLSL subset that builds as desktop GLSL 3.30 and as GLSL ES 3.00 (the
// Android GLES 3 backend, Phase 5B): only the #version line and precision statements differ.
#include "gl/glapi.h"

namespace post {

struct Settings {
    bool bloom = true;
    float bloom_intensity = 0.5f;   // 0..2: how much glow is added
    float bloom_threshold = 0.7f;   // 0..1: brightness where glow starts
    float bloom_radius = 1.0f;      // 0.5..2: blur size
    bool bloom_adaptive = true;     // by time of day: none by day, easing in at sunset, full at night
    float bloom_day = 0.0f;         // 0..1: daytime bloom when adaptive (fraction of the night's)
    bool fog = true;                // atmospheric fog: haze by distance in the screen pass (GL fog off)
    bool fog_palette = true;        // fog colours: Miami palette by time of day (a gradient) / the game's
    float fog_day = 0.15f;          // fog amount by day, at sunrise and sunset, at night (0..2)
    float fog_golden = 1.0f;
    float fog_night = 0.45f;
    float water_fog = 0.35f;        // 0..1: how much of the fog the sea gets
    float sunset_colour = 1.0f;     // 0..2: colour grade at sunrise / sunset (saturation, warm light, violet shade)
    bool height_fog = true;         // fog step 2: thicker near the ground / sea, sun haze (needs depth)
    float height_fog_amount = 1.0f; // 0..2
    bool sky_haze = true;           // the sky near the horizon fades into the fog colour (atmospheric fog)
    float muzzle_flash = 0.35f;     // 0..1: light from the player's muzzle flashes
    bool lights = true;             // dynamic lights from the game's glow sprites (light pools)
    float lights_intensity = 1.0f;  // 0..2: all dynamic lights
    float street_lights = 1.0f;     // 0..3: lamps and neon (pools on the street, walls)
    float street_light_range = 1.0f;  // 0.5..3: their reach
    float headlights = 1.0f;        // 0..3: car headlight beams (spot lights)
    float headlight_range = 30.0f;  // metres
    float headlight_width = 70.0f;  // degrees, the beam's full horizontal angle
    float light_distance = 200.0f;  // metres: how far away lamps still light the street
    bool light_coronas = true;      // distant lamps the game no longer draws show as points of light
    int max_lights = 128;           // per frame (the nearest / largest win)
    const char* test = nullptr;     // debugging (GANGSTAR_POST_TEST): "invert" | "depth"
};

// What the bridge knows about the scene that was just drawn.
struct Scene {
    GLuint fbo = 0;                 // the game's "screen" (the virtual framebuffer, maybe MSAA)
    int w = 0, h = 0;
    // Depth ranges and perspective projections (left-handed, Irrlicht style) used for the scene:
    // the near city in [0, split], the far (low-res) city and the sky in [split, 1].
    float split = 0.2f;
    float near_n = 50, near_f = 6000;
    float far_n = 6000, far_f = 30000;
    float tan_half_x = 0.7f, tan_half_y = 0.4f;  // from the near projection (1 / m[0], 1 / m[5])
    float view[16] = {};            // camera view matrix (column-major), world -> eye (the engine's camera)
    bool view_valid = false;
    float fog_color[3] = {0.6f, 0.65f, 0.7f};
    float fog_density = 0;          // EXP2 density the bridge uses (0 = no fog this frame)
    // The sea: drawn blended without depth, so the depth under it is the sea bed or the sky. Its pixels
    // are marked in the stencil (kWaterStencil) and the fog measures to its plane at this height.
    float water_z = 0;
    bool water_seen = false;        // a sea draw this frame (water_z is from it)
    // Dynamic lights (5C.6): every additive glow quad the game drew since the last atmosphere stage
    // (lamps, neon, headlights, taillights, police lights: the light-effects atlas). The atmosphere
    // stage runs before this frame's glows are drawn, so it uses the previous frame's: in world space.
    struct Glow {
        float pos[3];       // quad centre, world
        float size;         // largest extent (world units)
        float uv[4];        // its rectangle in the atlas, after the texture matrix (u0, v0, u1, v1)
        float origin[3];    // the draw's model origin, world (a car light's mount point)
        unsigned tex;       // the atlas (GL texture)
        bool world;         // drawn with the camera view: a city billboard (static); else a car's light
        int draw;           // draw call index this frame (the quads of one beam share it)
    };
    static constexpr int kMaxGlows = 1024;
    Glow glows[kMaxGlows] = {};
    int glow_count = 0;
};

bool init();                        // after the GL context exists: load entry points, build shaders
void set_settings(const Settings& s);
// Game clock (minutes since midnight) for the sun haze direction: the game's light 0 is a point at
// the origin, not the sun. World axes (from the minimap and the sea): +x east, +y north, +z up.
void set_time_of_day(int minute);
// A muzzle flash at a world position: a short orange light (dynamic lights on).
void flash(float x, float y, float z);
// The player's position (feet): its height is the reference for the ground fog. No player, or one
// far from the camera (menu scenes) = no ground fog.
void set_player(bool valid, const float pos[3]);
bool active();                      // any pass is on (else run() does nothing)
// The screen pass does the atmospheric fog (the bridge turns GL fog off for the scene).
bool screen_fog();
// The stencil bit the sea marks (the bridge sets it on the sea's draws; the atmosphere stage gives
// those pixels less fog). 0 = not used.
constexpr unsigned kWaterStencil = 0x80;

enum class Stage { Atmosphere, Finish, All };
// fixed_function: the game draws with desktop GL's fixed-function pipeline, whose extra state is saved
// with the attribute stacks; otherwise (shader renderer, GLES 3) only the state GLES 3 has is saved.
void run(const Scene& scene, bool fixed_function, Stage stage);

// GL fog for what is drawn after the atmosphere stage (glows, decals): EXP2 by eye distance, about the
// screen haze; additive glows fade to black instead (their colour is light, not a surface).
struct LateFog { float density = 0; float color[3] = {0, 0, 0}; };
LateFog late_fog();

}  // namespace post
