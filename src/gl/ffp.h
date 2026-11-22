#pragma once
// The GLES 1.1 fixed-function pipeline emulated with one shader (GLSL 3.30 on desktop, GLSL ES 3.00
// on Android), for the GLES 3 renderer (Phase 5B) and the desktop "shader" renderer that tests it.
//
// In shader mode the bridge (gles1.cpp) sends the game's fixed-function calls here instead of to GL:
// texture environments, alpha test, fog, current colour / texcoords, vertex arrays, clip planes,
// point size. Matrices stay in the bridge's shadow stacks and are passed per draw. Everything that
// GLES 3 still has (blending, depth, culling, stencil, scissor, viewport, textures, buffers,
// framebuffers) is still set on GL directly.
//
// Not emulated (the game never uses them, checked with traces): lighting (GL_LIGHTING is never
// enabled; the engine lights on the CPU), flat shading, logic ops, line/point smoothing.
#include <SDL_opengl.h>

#include <cstdint>

namespace ffp {

constexpr int kUnits = 3;  // texture units offered to the game in shader mode

bool init();                // compile the shader; needs a GL 3.3 / GLES 3.0 context
bool ready();
// Someone else (screen effects, the overlay) changed the program / vertex arrays: re-apply on the
// next draw.
void invalidate();

// ---- state (mirrors of the GLES 1.1 calls) --------------------------------------------------------
bool is_ffp_cap(GLenum cap);                  // a cap that only exists in the fixed-function pipeline
void enable(GLenum cap, bool on, int unit);   // GL_TEXTURE_2D is per texture unit
bool enabled(GLenum cap, int unit);
void client_state(GLenum array, bool on, int client_unit);
bool array_enabled(GLenum array);             // GL_VERTEX_ARRAY / GL_COLOR_ARRAY / GL_NORMAL_ARRAY
void color(float r, float g, float b, float a);
void multi_texcoord(int unit, float s, float t, float r, float q);
struct Array {
    GLint size = 4;
    GLenum type = GL_FLOAT;
    GLsizei stride = 0;
    const void* ptr = nullptr;   // host pointer (client array) or offset (buffer bound)
    GLuint buffer = 0;
};
void vertex_pointer(const Array& a);
void color_pointer(const Array& a);
void texcoord_pointer(int unit, const Array& a);
void tex_env(int unit, GLenum target, GLenum pname, const float* v, int n);
void fog(GLenum pname, const float* v, int n);
void alpha_func(GLenum func, float ref);
void point_size(float s);
void clip_plane(int i, const float eye_plane[4]);  // already in eye space (GL semantics)
// The bridge's own additions: fog forced on for this draw, the time-of-day tint (nullptr = none).
void set_fog_forced(bool on);
void set_tint(const float* rgb);
// Base format of a texture (GL_ALPHA, GL_LUMINANCE, GL_LUMINANCE_ALPHA, GL_RGB, GL_RGBA): the
// texture environment depends on it.
void set_texture_format(GLuint tex, GLenum base_format);
void forget_texture(GLuint tex);

// ---- drawing ---------------------------------------------------------------------------------------
struct DrawState {
    const float* proj;            // column-major 4x4
    const float* modelview;
    const float* texmat[kUnits];
    const GLuint* bound;          // GL_TEXTURE_2D bound per unit (the game's)
    GLuint game_array_buffer;     // restored after the attribute setup
};
void draw_arrays(GLenum mode, GLint first, GLsizei count, const DrawState& s);
void draw_elements(GLenum mode, GLsizei count, GLenum type, const void* indices, const DrawState& s);

}  // namespace ffp
