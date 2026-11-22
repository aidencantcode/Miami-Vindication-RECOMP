#pragma once
// Screen effects on the 3D scene only (Phase 5C.0): the GL bridge calls run() at the end of the
// scene, just before the first HUD (orthographic) draw of the frame, or at present when nothing 2D
// follows. The scene's colour and depth are copied (MSAA resolved) into textures, the passes draw
// the result back into the game's framebuffer, and all GL state the game relies on is restored, so
// the HUD draws on top unchanged.
//
// Shaders are written in a GLSL subset that builds as desktop GLSL 3.30 and as GLSL ES 3.00 (the
// Android GLES 3 backend, Phase 5B): only the #version line and precision statements differ.
#include <SDL_opengl.h>

namespace post {

struct Settings {
    bool bloom = true;
    float bloom_intensity = 0.5f;   // 0..2: how much glow is added
    float bloom_threshold = 0.7f;   // 0..1: brightness where glow starts
    float bloom_radius = 1.0f;      // 0.5..2: blur size
    bool height_fog = true;         // fog step 2: thicker near the ground / sea, sun haze (needs depth)
    float height_fog_amount = 1.0f; // 0..2
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
    float view[16] = {};            // camera view matrix (column-major), world -> eye, from a city draw
    bool view_valid = false;
    float fog_color[3] = {0.6f, 0.65f, 0.7f};
    float fog_density = 0;          // EXP2 density the bridge uses (0 = no fog this frame)
};

bool init();                        // after the GL context exists: load entry points, build shaders
void set_settings(const Settings& s);
bool active();                      // any pass is on (else run() does nothing)
void run(const Scene& scene);

}  // namespace post
