#pragma once
// OpenGL ES 1.1 (as used by the game) -> desktop OpenGL compatibility profile.
// The probe's 2a trace defines what is needed: 64 of 95 imported GL functions during
// boot + first gameplay load, FLOAT/UBYTE vertex arrays (no GL_FIXED), ushort indices,
// DXT1/DXT5 compressed textures, point sprites.
#include <cstdint>

namespace rt { class Runtime; }

namespace gles1 {

bool load_gl();                     // after a GL context is current
void register_handlers(rt::Runtime& rt);
uint64_t draw_calls();              // statistics
void report();
// Read back the window's back buffer and save it as a BMP.
bool screenshot(const char* path, int w, int h);

// ---- Virtual default framebuffer ---------------------------------------------------------------
// The game draws "to the screen" (framebuffer 0) at its logical size (e.g. 854x480).
// We redirect framebuffer 0 to an offscreen FBO of any size (the internal resolution: an
// integer multiple of the logical size, or exactly the size it is shown at in the window),
// map the game's viewport/scissor/point/line sizes onto it, and present the result into the
// window. The game never notices; the GPU renders at the internal resolution.
struct VfbConfig {
    int logical_w = 800, logical_h = 480;  // what the game is told (height must stay 480)
    int width = 800, height = 480;         // internal resolution in pixels
    int msaa = 0;                          // 0, 2, 4, 8 samples
};
bool vfb_init(const VfbConfig& cfg);
// Live change of the internal resolution and MSAA (re-creates the FBO; falls back to no MSAA).
bool vfb_reconfigure(int width, int height, int msaa);

// Anisotropic filtering for mipmapped textures (applied when the game sets a texture's
// minification filter, i.e. as textures load). 1 = off.
void set_anisotropy(int level);

// Texture filtering override (applied live to every texture whose filters the game set).
enum class TextureFilter { Game, Nearest, Bilinear, Trilinear };
void set_texture_filter(TextureFilter f);
const VfbConfig& vfb_config();
enum class Fit { Aspect, Stretch, Integer };
// The rectangle (x, y from the top, w, h) a logical_w x logical_h image occupies in a win_w x win_h window.
void fit_rect(int logical_w, int logical_h, int win_w, int win_h, Fit fit, int* x, int* y, int* w, int* h);
// Copy the game image into the window (w x h); returns the rectangle used (for input mapping).
void vfb_present(int win_w, int win_h, Fit fit, bool linear, int* rx, int* ry, int* rw, int* rh);
// Re-bind whatever framebuffer the game had bound (after present/overlay drawing).
void vfb_restore_binding();

// Solid rectangles drawn into the window after vfb_present (window pixels, y from the top).
// Drawn with scissored clears, so no game GL state (textures, blending, matrices) is touched.
struct OverlayRect { int x, y, w, h; float r, g, b; };
void draw_overlay_rects(int win_w, int win_h, const OverlayRect* rects, int n);

}  // namespace gles1
