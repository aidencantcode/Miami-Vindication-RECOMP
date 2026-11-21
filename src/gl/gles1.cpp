#include "gl/gles1.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "rt/runtime.h"

namespace gles1 {

using cpu::Args;
using cpu::Cpu;

namespace {

// ---- entry points beyond OpenGL 1.1 (loaded at runtime) -------------------------------------------
// glext.h only declares core-profile typedefs; these two are compatibility-profile only.
typedef void(APIENTRY* PFNGLCLIENTACTIVETEXTUREPROC)(GLenum texture);
typedef void(APIENTRY* PFNGLMULTITEXCOORD4FPROC)(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q);

#define GL_EXT_FUNCS(X)                                                                                  \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                                                           \
    X(PFNGLCLIENTACTIVETEXTUREPROC, glClientActiveTexture)                                               \
    X(PFNGLMULTITEXCOORD4FPROC, glMultiTexCoord4f)                                                       \
    X(PFNGLCOMPRESSEDTEXIMAGE2DPROC, glCompressedTexImage2D)                                             \
    X(PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC, glCompressedTexSubImage2D)                                       \
    X(PFNGLSAMPLECOVERAGEPROC, glSampleCoverage)                                                         \
    X(PFNGLPOINTPARAMETERFPROC, glPointParameterf)                                                       \
    X(PFNGLPOINTPARAMETERFVPROC, glPointParameterfv)                                                     \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation)                                                           \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                                                 \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                                                 \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                                                           \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                                                 \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                                                           \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                                       \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)                                                     \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                                         \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                                                 \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers)                                               \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)                                       \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                                             \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                                                       \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers)                                                     \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap)                                                         \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)                                               \
    X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample)                         \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)

#define DECLARE(type, name) type p_##name = nullptr;
GL_EXT_FUNCS(DECLARE)
#undef DECLARE

// The identity the engine expects (it parses GL_VERSION at offset 13 for "1.1").
const char* kVendor = "NVIDIA Corporation";
const char* kRenderer = "NVIDIA Tegra";
const char* kVersion = "OpenGL ES-CM 1.1";
const char* kExtensions =
    "GL_ARB_texture_env_combine GL_ARB_texture_env_dot3 GL_EXT_texture_compression_dxt1 "
    "GL_EXT_texture_compression_s3tc GL_EXT_texture_format_BGRA8888 GL_EXT_texture_lod_bias "
    "GL_NV_texture_compression_s3tc GL_OES_blend_equation_separate GL_OES_blend_func_separate "
    "GL_OES_blend_subtract GL_OES_byte_coordinates GL_OES_compressed_ETC1_RGB8_texture "
    "GL_OES_compressed_paletted_texture GL_OES_depth24 GL_OES_draw_texture GL_OES_EGL_image "
    "GL_OES_element_index_uint GL_OES_extended_matrix_palette GL_OES_fbo_render_mipmap GL_OES_fixed_point "
    "GL_OES_framebuffer_object GL_OES_mapbuffer GL_OES_matrix_get GL_OES_matrix_palette "
    "GL_OES_packed_depth_stencil GL_OES_point_size_array GL_OES_point_sprite GL_OES_query_matrix "
    "GL_OES_read_format GL_OES_rgb8_rgba8 GL_OES_single_precision GL_OES_stencil8 GL_OES_texture_cube_map "
    "GL_OES_texture_mirrored_repeat";
std::unordered_map<uint32_t, uint32_t> g_strings;  // GL name -> guest pointer

// ---- virtual default framebuffer (see gles1.h) -------------------------------------------------------
VfbConfig g_cfg;
GLuint g_vfb = 0, g_vfb_color = 0, g_vfb_depth = 0;      // what the game sees as framebuffer 0
GLuint g_resolve_fb = 0, g_resolve_color = 0;            // MSAA resolve target (same size, 1 sample)
GLuint g_read_fb = 0, g_read_color = 0;                  // logical-size target for glReadPixels
GLuint g_game_fb = 0;                                    // framebuffer the game thinks is bound
float g_aniso = 1;                                       // anisotropic filtering level (1 = off)
bool on_screen() { return g_game_fb == 0; }
// "Screen" coordinates (logical_w x logical_h) -> internal pixels. Any ratio works (2.25 at 1080p).
double kx() { return on_screen() ? double(g_cfg.width) / g_cfg.logical_w : 1.0; }
double ky() { return on_screen() ? double(g_cfg.height) / g_cfg.logical_h : 1.0; }
// Map a rectangle by its edges, so rectangles that touch still touch at fractional ratios.
void map_rect(const GLint r[4], GLint* x, GLint* y, GLsizei* w, GLsizei* h) {
    const double sx = kx(), sy = ky();
    const GLint x0 = GLint(std::lround(r[0] * sx)), x1 = GLint(std::lround((double(r[0]) + r[2]) * sx));
    const GLint y0 = GLint(std::lround(r[1] * sy)), y1 = GLint(std::lround((double(r[1]) + r[3]) * sy));
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}
// Viewport and scissor box as the game set them (its own coordinates). Queries return these, and
// they are re-mapped whenever the game switches between the screen and its own framebuffers,
// as they would be on a real logical_w x logical_h screen.
GLint g_viewport[4] = {0, 0, 800, 480}, g_scissor[4] = {0, 0, 800, 480};
void apply_viewport() { GLint x, y; GLsizei w, h; map_rect(g_viewport, &x, &y, &w, &h); glViewport(x, y, w, h); }
void apply_scissor() { GLint x, y; GLsizei w, h; map_rect(g_scissor, &x, &y, &w, &h); glScissor(x, y, w, h); }

void make_rb(GLuint* rb, GLenum fmt, int w, int h, int samples) {
    if (!*rb) p_glGenRenderbuffers(1, rb);
    p_glBindRenderbuffer(GL_RENDERBUFFER, *rb);
    if (samples) p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, fmt, w, h);
    else p_glRenderbufferStorage(GL_RENDERBUFFER, fmt, w, h);
}

bool build_vfb() {
    const int w = g_cfg.width, h = g_cfg.height;
    if (!g_vfb) p_glGenFramebuffers(1, &g_vfb);
    make_rb(&g_vfb_color, GL_RGBA8, w, h, g_cfg.msaa);
    make_rb(&g_vfb_depth, GL_DEPTH24_STENCIL8, w, h, g_cfg.msaa);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_vfb);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_vfb_color);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_vfb_depth);
    GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (g_cfg.msaa) {
        if (!g_resolve_fb) p_glGenFramebuffers(1, &g_resolve_fb);
        make_rb(&g_resolve_color, GL_RGBA8, w, h, 0);
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_resolve_fb);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_resolve_color);
    }
    if (!g_read_fb) p_glGenFramebuffers(1, &g_read_fb);
    make_rb(&g_read_color, GL_RGBA8, g_cfg.logical_w, g_cfg.logical_h, 0);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_read_fb);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_read_color);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_game_fb ? g_game_fb : g_vfb);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        LOG("GL: virtual framebuffer %dx%d (msaa %d) incomplete: %#x", w, h, g_cfg.msaa, st);
        return false;
    }
    LOG("GL: virtual framebuffer %dx%d (logical %dx%d x%.3g, msaa %d)", w, h, g_cfg.logical_w, g_cfg.logical_h,
        double(h) / g_cfg.logical_h, g_cfg.msaa);
    if (on_screen()) {  // the game's viewport/scissor in the new internal pixels
        apply_viewport();
        apply_scissor();
    }
    return true;
}

// The single-sample framebuffer holding the current "screen" image (resolving MSAA if needed).
GLuint screen_source() {
    if (!g_cfg.msaa) return g_vfb;
    const int w = g_cfg.width, h = g_cfg.height;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_vfb);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_resolve_fb);
    p_glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    return g_resolve_fb;
}

void bind_game_fb() { p_glBindFramebuffer(GL_FRAMEBUFFER, g_game_fb ? g_game_fb : g_vfb); }

// Buffer-object state needed to interpret pointer arguments.
uint32_t g_array_buffer = 0, g_element_buffer = 0;
std::unordered_map<uint32_t, uint32_t> g_buffer_size;                    // buffer -> size
std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> g_shadow;    // buffer -> (guest ptr, capacity)
uint64_t g_draws = 0;

#define H(fn) void fn(Cpu& c, Args& a)
#define M (c.rt().mem())

const void* host(Cpu& c, uint32_t p) { return p ? M.ptr(p) : nullptr; }
// Vertex-array / index pointers: offsets when a buffer object is bound, guest pointers otherwise.
const void* array_ptr(Cpu& c, uint32_t p, bool bound) {
    return bound ? reinterpret_cast<const void*>(uintptr_t(p)) : host(c, p);
}

// ---- state -----------------------------------------------------------------------------------------
H(enable) { glEnable(a.u32()); }
H(disable) { glDisable(a.u32()); }
H(enable_cs) { glEnableClientState(a.u32()); }
H(disable_cs) { glDisableClientState(a.u32()); }
H(alpha_func) { GLenum f = a.u32(); float r = a.f32(); glAlphaFunc(f, r); }
H(blend_func) { GLenum s = a.u32(), d = a.u32(); glBlendFunc(s, d); }
H(blend_equation) { p_glBlendEquation(a.u32()); }
H(color_mask) { GLboolean r = GLboolean(a.u32()), g = GLboolean(a.u32()), b = GLboolean(a.u32()), al = GLboolean(a.u32()); glColorMask(r, g, b, al); }
H(cull_face) { glCullFace(a.u32()); }
H(front_face) { glFrontFace(a.u32()); }
H(depth_func) { glDepthFunc(a.u32()); }
H(depth_mask) { glDepthMask(GLboolean(a.u32())); }
H(depth_rangef) { float n = a.f32(), f = a.f32(); glDepthRange(n, f); }
H(polygon_offset) { float f = a.f32(), u = a.f32(); glPolygonOffset(f, u); }
H(line_width) { glLineWidth(float(a.f32() * ky())); }
H(point_size) { glPointSize(float(a.f32() * ky())); }
H(point_param_f) {
    GLenum p = a.u32(); float v = a.f32();
    if (p == GL_POINT_SIZE_MIN || p == GL_POINT_SIZE_MAX) v = float(v * ky());
    p_glPointParameterf(p, v);
}
H(point_param_fv) { GLenum p = a.u32(); p_glPointParameterfv(p, static_cast<const GLfloat*>(host(c, a.u32()))); }
H(sample_coverage) { float v = a.f32(); GLboolean inv = GLboolean(a.u32()); p_glSampleCoverage(v, inv); }
H(scissor) { for (GLint& v : g_scissor) v = a.s32(); apply_scissor(); }
H(viewport) { for (GLint& v : g_viewport) v = a.s32(); apply_viewport(); }
H(shade_model) { glShadeModel(a.u32()); }
H(hint) { GLenum t = a.u32(), m = a.u32(); glHint(t, m); }
H(stencil_func) { GLenum f = a.u32(); GLint r = a.s32(); GLuint m = a.u32(); glStencilFunc(f, r, m); }
H(stencil_mask) { glStencilMask(a.u32()); }
H(stencil_op) { GLenum f = a.u32(), zf = a.u32(), zp = a.u32(); glStencilOp(f, zf, zp); }
H(pixel_storei) { GLenum p = a.u32(); GLint v = a.s32(); glPixelStorei(p, v); }
H(clear) { glClear(a.u32()); }
H(clear_color) { float r = a.f32(), g = a.f32(), b = a.f32(), al = a.f32(); glClearColor(r, g, b, al); }
H(clear_depthf) { glClearDepth(a.f32()); }
H(clear_stencil) { glClearStencil(a.s32()); }
H(flush) { glFlush(); }
H(clip_planef) {
    GLenum p = a.u32();
    const float* e = static_cast<const float*>(host(c, a.u32()));
    const double d[4] = {e[0], e[1], e[2], e[3]};
    glClipPlane(p, d);
}
H(get_error) { c.ret(glGetError()); }
H(get_string) {
    uint32_t name = a.u32();
    const char* s = name == GL_VENDOR ? kVendor : name == GL_RENDERER ? kRenderer : name == GL_VERSION ? kVersion
                  : name == GL_EXTENSIONS ? kExtensions : nullptr;
    if (!s) return c.ret(0);
    auto it = g_strings.find(name);
    if (it == g_strings.end()) {
        uint32_t p = c.rt().heap().alloc(uint32_t(std::strlen(s)) + 1);
        M.write(p, s, std::strlen(s) + 1);
        it = g_strings.emplace(name, p).first;
    }
    c.ret(it->second);
}
void tint_before_draw();  // world tint stage (time of day), defined with the texture state below
int tint_unit();          // texture unit of that stage (-1: none); hidden from the game
H(get_integerv) {
    GLenum p = a.u32();
    GLint* out = static_cast<GLint*>(const_cast<void*>(host(c, a.u32())));
    if (p == GL_FRAMEBUFFER_BINDING) { *out = GLint(g_game_fb); return; }
    if (p == GL_MAX_TEXTURE_UNITS && tint_unit() > 0) {  // the last unit is the time-of-day tint's
        glGetIntegerv(p, out);
        *out = std::min(*out, GLint(tint_unit()));
        return;
    }
    if (p == GL_VIEWPORT || p == GL_SCISSOR_BOX) {  // the game's own values, not internal pixels
        std::memcpy(out, p == GL_VIEWPORT ? g_viewport : g_scissor, sizeof g_viewport);
        return;
    }
    glGetIntegerv(p, out);
}
H(get_floatv) {
    GLenum p = a.u32();
    GLfloat* out = static_cast<GLfloat*>(const_cast<void*>(host(c, a.u32())));
    if (p == GL_VIEWPORT || p == GL_SCISSOR_BOX) {
        for (int i = 0; i < 4; ++i) out[i] = float(p == GL_VIEWPORT ? g_viewport[i] : g_scissor[i]);
        return;
    }
    glGetFloatv(p, out);
}

// ---- fixed-function pipeline ---------------------------------------------------------------------
H(matrix_mode) { glMatrixMode(a.u32()); }
H(load_identity) { glLoadIdentity(); }
H(load_matrixf) { glLoadMatrixf(static_cast<const GLfloat*>(host(c, a.u32()))); }
H(mult_matrixf) { glMultMatrixf(static_cast<const GLfloat*>(host(c, a.u32()))); }
H(push_matrix) { glPushMatrix(); }
H(pop_matrix) { glPopMatrix(); }
H(color4f) { float r = a.f32(), g = a.f32(), b = a.f32(), al = a.f32(); glColor4f(r, g, b, al); }
H(color4ub) { GLubyte r = GLubyte(a.u32()), g = GLubyte(a.u32()), b = GLubyte(a.u32()), al = GLubyte(a.u32()); glColor4ub(r, g, b, al); }
H(normal3f) { float x = a.f32(), y = a.f32(), z = a.f32(); glNormal3f(x, y, z); }
H(multi_texcoord4f) { GLenum t = a.u32(); float s = a.f32(), u = a.f32(), r = a.f32(), q = a.f32(); p_glMultiTexCoord4f(t, s, u, r, q); }
// GANGSTAR_GL_LIGHT_TRACE=1: log each distinct lighting/fog/texenv call once (day/night debugging).
void light_trace(const char* fn, GLenum a, GLenum b, const float* v, int n) {
    static const bool on = [] { const char* e = std::getenv("GANGSTAR_GL_LIGHT_TRACE"); return e && *e && *e != '0'; }();
    if (!on) return;
    static std::unordered_map<std::string, int> seen;
    char buf[200];
    int k = std::snprintf(buf, sizeof buf, "%s(%#x, %#x", fn, a, b);
    for (int i = 0; i < n && k < int(sizeof buf) - 16; ++i) k += std::snprintf(buf + k, sizeof buf - k, ", %.3f", v[i]);
    std::snprintf(buf + k, sizeof buf - k, ")");
    if (seen[buf]++ == 0 && seen.size() < 400) LOG("gl-light %s", buf);
}
int param_count(GLenum p) {
    switch (p) {
        case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION: case GL_EMISSION:
        case GL_AMBIENT_AND_DIFFUSE: case GL_LIGHT_MODEL_AMBIENT: case GL_FOG_COLOR: case GL_TEXTURE_ENV_COLOR: return 4;
        case GL_SPOT_DIRECTION: return 3;
        default: return 1;
    }
}
H(fogf) { GLenum p = a.u32(); float v = a.f32(); light_trace("glFogf", p, 0, &v, 1); glFogf(p, v); }
H(fogfv) { GLenum p = a.u32(); auto v = static_cast<const GLfloat*>(host(c, a.u32())); light_trace("glFogfv", p, 0, v, param_count(p)); glFogfv(p, v); }
H(lightf) { GLenum l = a.u32(), p = a.u32(); float v = a.f32(); light_trace("glLightf", l, p, &v, 1); glLightf(l, p, v); }
H(lightfv) { GLenum l = a.u32(), p = a.u32(); auto v = static_cast<const GLfloat*>(host(c, a.u32())); light_trace("glLightfv", l, p, v, param_count(p)); glLightfv(l, p, v); }
H(light_modelf) { GLenum p = a.u32(); float v = a.f32(); light_trace("glLightModelf", p, 0, &v, 1); glLightModelf(p, v); }
H(light_modelfv) { GLenum p = a.u32(); auto v = static_cast<const GLfloat*>(host(c, a.u32())); light_trace("glLightModelfv", p, 0, v, param_count(p)); glLightModelfv(p, v); }
H(materialf) { GLenum f = a.u32(), p = a.u32(); float v = a.f32(); light_trace("glMaterialf", f, p, &v, 1); glMaterialf(f, p, v); }
H(materialfv) { GLenum f = a.u32(), p = a.u32(); auto v = static_cast<const GLfloat*>(host(c, a.u32())); light_trace("glMaterialfv", f, p, v, param_count(p)); glMaterialfv(f, p, v); }
H(tex_envf) { GLenum t = a.u32(), p = a.u32(); float v = a.f32(); light_trace("glTexEnvf", t, p, &v, 1); glTexEnvf(t, p, v); }
H(tex_envi) { GLenum t = a.u32(), p = a.u32(); GLint v = a.s32(); float fv = float(v); light_trace("glTexEnvi", t, p, &fv, 1); glTexEnvi(t, p, v); }
H(tex_envfv) { GLenum t = a.u32(), p = a.u32(); auto v = static_cast<const GLfloat*>(host(c, a.u32())); light_trace("glTexEnvfv", t, p, v, param_count(p)); glTexEnvfv(t, p, v); }

// ---- vertex arrays and drawing ---------------------------------------------------------------------
H(vertex_pointer) { GLint s = a.s32(); GLenum t = a.u32(); GLsizei st = a.s32(); glVertexPointer(s, t, st, array_ptr(c, a.u32(), g_array_buffer)); }
H(texcoord_pointer) { GLint s = a.s32(); GLenum t = a.u32(); GLsizei st = a.s32(); glTexCoordPointer(s, t, st, array_ptr(c, a.u32(), g_array_buffer)); }
H(color_pointer) { GLint s = a.s32(); GLenum t = a.u32(); GLsizei st = a.s32(); glColorPointer(s, t, st, array_ptr(c, a.u32(), g_array_buffer)); }
H(normal_pointer) { GLenum t = a.u32(); GLsizei st = a.s32(); glNormalPointer(t, st, array_ptr(c, a.u32(), g_array_buffer)); }
H(client_active_texture) { p_glClientActiveTexture(a.u32()); }
H(draw_arrays) { GLenum m = a.u32(); GLint f = a.s32(); GLsizei n = a.s32(); tint_before_draw(); glDrawArrays(m, f, n); ++g_draws; }
H(draw_elements) {
    GLenum m = a.u32(); GLsizei n = a.s32(); GLenum t = a.u32();
    tint_before_draw();
    glDrawElements(m, n, t, array_ptr(c, a.u32(), g_element_buffer));
    ++g_draws;
}

// ---- buffers ------------------------------------------------------------------------------------------
H(gen_buffers) { GLsizei n = a.s32(); p_glGenBuffers(n, static_cast<GLuint*>(const_cast<void*>(host(c, a.u32())))); }
H(delete_buffers) {
    GLsizei n = a.s32();
    const GLuint* ids = static_cast<const GLuint*>(host(c, a.u32()));
    for (GLsizei i = 0; i < n; ++i) {
        if (ids[i] == g_array_buffer) g_array_buffer = 0;
        if (ids[i] == g_element_buffer) g_element_buffer = 0;
        g_buffer_size.erase(ids[i]);
    }
    p_glDeleteBuffers(n, ids);
}
H(bind_buffer) {
    GLenum t = a.u32(); GLuint b = a.u32();
    if (t == GL_ARRAY_BUFFER) g_array_buffer = b;
    if (t == GL_ELEMENT_ARRAY_BUFFER) g_element_buffer = b;
    p_glBindBuffer(t, b);
}
uint32_t bound_buffer(GLenum t) { return t == GL_ARRAY_BUFFER ? g_array_buffer : t == GL_ELEMENT_ARRAY_BUFFER ? g_element_buffer : 0; }
H(buffer_data) {
    GLenum t = a.u32(); GLsizeiptr n = GLsizeiptr(a.u32()); uint32_t d = a.u32(); GLenum u = a.u32();
    g_buffer_size[bound_buffer(t)] = uint32_t(n);
    p_glBufferData(t, n, host(c, d), u);
}
H(buffer_subdata) { GLenum t = a.u32(); GLintptr o = GLintptr(a.u32()); GLsizeiptr n = GLsizeiptr(a.u32()); p_glBufferSubData(t, o, n, host(c, a.u32())); }
H(map_buffer) {
    // OES_mapbuffer is write-only: hand out a guest-memory shadow, upload it on unmap.
    GLenum t = a.u32();
    uint32_t b = bound_buffer(t), size = g_buffer_size[b];
    auto& sh = g_shadow[b];
    if (sh.second < size) sh = {c.rt().heap().alloc(size), size};
    c.ret(sh.first);
}
H(unmap_buffer) {
    GLenum t = a.u32();
    uint32_t b = bound_buffer(t);
    auto it = g_shadow.find(b);
    if (it != g_shadow.end()) p_glBufferSubData(t, 0, g_buffer_size[b], M.ptr(it->second.first));
    c.ret(GL_TRUE);
}

// ---- textures -----------------------------------------------------------------------------------------
// Texture filtering override. We remember the filters the GAME asked for per texture and apply the
// user's mode on top: the game's mip / no-mip choice is kept (a mipmapped filter on a texture
// without mipmaps would make it render black), only nearest / linear sampling changes.
struct TexFilters { GLint min = GL_NEAREST_MIPMAP_LINEAR, mag = GL_LINEAR; };
std::unordered_map<GLuint, TexFilters> g_tex_filters;  // textures whose filters the game set
TextureFilter g_filter_mode = TextureFilter::Game;
GLuint g_bound_2d[8] = {};                              // GL_TEXTURE_2D binding per texture unit
int g_unit = 0;

bool is_mip(GLint f) {
    return f == GL_NEAREST_MIPMAP_NEAREST || f == GL_LINEAR_MIPMAP_NEAREST || f == GL_NEAREST_MIPMAP_LINEAR ||
           f == GL_LINEAR_MIPMAP_LINEAR;
}
GLint effective_min(GLint f) {
    switch (g_filter_mode) {
        case TextureFilter::Nearest: return is_mip(f) ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
        case TextureFilter::Bilinear: return is_mip(f) ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
        case TextureFilter::Trilinear: return is_mip(f) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
        default: return f;
    }
}
GLint effective_mag(GLint f) {
    if (g_filter_mode == TextureFilter::Game) return f;
    return g_filter_mode == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR;
}
// Applies filters + anisotropy (mipmapped, non-nearest textures only) to the bound 2D texture.
void apply_filters(GLenum target, const TexFilters& t) {
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, effective_min(t.min));
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, effective_mag(t.mag));
    const bool aniso = is_mip(t.min) && g_filter_mode != TextureFilter::Nearest;
    glTexParameterf(target, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, aniso ? g_aniso : 1.0f);
}
void reapply_all_filters() {
    for (const auto& [id, f] : g_tex_filters) {
        glBindTexture(GL_TEXTURE_2D, id);
        apply_filters(GL_TEXTURE_2D, f);
    }
    glBindTexture(GL_TEXTURE_2D, g_bound_2d[g_unit]);
}

TextureObserver g_tex_observer;

// ---- world tint (time of day, game/night_textures) ----------------------------------------------
// An extra fixed-function texture stage after the game's own: MODULATE(previous, constant colour),
// enabled only for draws whose unit-0 texture is a registered world texture (city, ground), so
// characters, cars, the sky and the HUD keep the game's own colours.
float g_tint[3] = {1, 1, 1};
std::unordered_set<GLuint> g_world_tex;
int g_tint_unit = -1;  // fixed-function unit used for the stage (-1: none spare)
bool g_tint_on = false, g_tint_dirty = true;

int tint_unit() { return g_tint_unit; }

void tint_init() {
    GLint units = 0;
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    if (units < 3) { LOG("GL: no spare texture unit for the time-of-day tint (%d units)", units); return; }
    g_tint_unit = std::min(int(units), 8) - 1;
    GLuint white = 0;
    glGenTextures(1, &white);
    p_glActiveTexture(GL_TEXTURE0 + g_tint_unit);
    glBindTexture(GL_TEXTURE_2D, white);
    const uint8_t px[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_CONSTANT);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
    glDisable(GL_TEXTURE_2D);
    p_glActiveTexture(GL_TEXTURE0 + g_unit);
    LOG("GL: time-of-day tint on texture unit %d", g_tint_unit);
}

void tint_before_draw() {
    if (g_tint_unit < 0) return;
    const bool tinted = std::abs(g_tint[0] - 1) + std::abs(g_tint[1] - 1) + std::abs(g_tint[2] - 1) > 0.003f;
    const bool want = tinted && g_world_tex.count(g_bound_2d[0]) != 0;
    if (want == g_tint_on && !(want && g_tint_dirty)) return;
    p_glActiveTexture(GL_TEXTURE0 + g_tint_unit);
    if (want) {
        if (!g_tint_on) glEnable(GL_TEXTURE_2D);
        if (g_tint_dirty) {
            const GLfloat c[4] = {g_tint[0], g_tint[1], g_tint[2], 1};
            glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, c);
            g_tint_dirty = false;
        }
    } else {
        glDisable(GL_TEXTURE_2D);
    }
    static const bool debug = std::getenv("GANGSTAR_TINT_DEBUG") != nullptr;
    if (debug) {
        static int logged = 0;
        if (want && logged < 6) {
            GLfloat c[4] = {}, mode = 0, src1 = 0, comb = 0;
            GLint unit_bound = 0;
            glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, c);
            glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &mode);
            glGetTexEnvfv(GL_TEXTURE_ENV, GL_SOURCE1_RGB, &src1);
            glGetTexEnvfv(GL_TEXTURE_ENV, GL_COMBINE_RGB, &comb);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit_bound);
            LOG("tint-debug: unit %d env color %.2f %.2f %.2f mode %#x combine %#x src1 %#x bound %d enabled %d (tint %.2f %.2f %.2f)",
                g_tint_unit, c[0], c[1], c[2], unsigned(mode), unsigned(comb), unsigned(src1), unit_bound,
                int(glIsEnabled(GL_TEXTURE_2D)), g_tint[0], g_tint[1], g_tint[2]);
            ++logged;
        }
    }
    p_glActiveTexture(GL_TEXTURE0 + g_unit);
    g_tint_on = want;
}

H(gen_textures) { GLsizei n = a.s32(); glGenTextures(n, static_cast<GLuint*>(const_cast<void*>(host(c, a.u32())))); }
H(delete_textures) {
    GLsizei n = a.s32();
    const GLuint* ids = static_cast<const GLuint*>(host(c, a.u32()));
    for (GLsizei i = 0; i < n; ++i) {
        if (g_tex_observer.on_delete) g_tex_observer.on_delete(ids[i]);
        g_tex_filters.erase(ids[i]);
        for (GLuint& b : g_bound_2d)
            if (b == ids[i]) b = 0;
    }
    glDeleteTextures(n, ids);
}
H(bind_texture) {
    GLenum t = a.u32();
    GLuint id = a.u32();
    if (t == GL_TEXTURE_2D) g_bound_2d[g_unit] = id;
    glBindTexture(t, id);
}
H(active_texture) {
    GLenum u = a.u32();
    g_unit = std::clamp(int(u) - int(GL_TEXTURE0), 0, 7);
    p_glActiveTexture(u);
}
void tex_param(GLenum t, GLenum p, GLint v) {
    if (t == GL_TEXTURE_2D && (p == GL_TEXTURE_MIN_FILTER || p == GL_TEXTURE_MAG_FILTER)) {
        TexFilters& f = g_tex_filters[g_bound_2d[g_unit]];
        (p == GL_TEXTURE_MIN_FILTER ? f.min : f.mag) = v;
        apply_filters(t, f);
        return;
    }
    glTexParameteri(t, p, v);
}
H(tex_parameteri) { GLenum t = a.u32(), p = a.u32(); GLint v = a.s32(); tex_param(t, p, v); }
H(tex_parameterf) {
    GLenum t = a.u32(), p = a.u32();
    float v = a.f32();
    if (p == GL_TEXTURE_MIN_FILTER || p == GL_TEXTURE_MAG_FILTER) tex_param(t, p, GLint(v));
    else glTexParameterf(t, p, v);
}
H(tex_image_2d) {
    GLenum t = a.u32(); GLint l = a.s32(), ifmt = a.s32(); GLsizei w = a.s32(), h = a.s32(); GLint b = a.s32();
    GLenum f = a.u32(), ty = a.u32();
    glTexImage2D(t, l, ifmt, w, h, b, f, ty, host(c, a.u32()));
}
H(tex_subimage_2d) {
    GLenum t = a.u32(); GLint l = a.s32(), x = a.s32(), y = a.s32(); GLsizei w = a.s32(), h = a.s32();
    GLenum f = a.u32(), ty = a.u32();
    glTexSubImage2D(t, l, x, y, w, h, f, ty, host(c, a.u32()));
}
H(compressed_tex_image_2d) {
    GLenum t = a.u32(); GLint l = a.s32(); GLenum ifmt = a.u32(); GLsizei w = a.s32(), h = a.s32(); GLint b = a.s32();
    GLsizei n = a.s32();
    const void* data = host(c, a.u32());
    p_glCompressedTexImage2D(t, l, ifmt, w, h, b, n, data);
    if (t == GL_TEXTURE_2D && g_tex_observer.on_compressed_upload)
        g_tex_observer.on_compressed_upload(g_bound_2d[g_unit], l, ifmt, w, h, static_cast<const uint8_t*>(data), n);
}
H(compressed_tex_subimage_2d) {
    GLenum t = a.u32(); GLint l = a.s32(), x = a.s32(), y = a.s32(); GLsizei w = a.s32(), h = a.s32();
    GLenum f = a.u32(); GLsizei n = a.s32();
    p_glCompressedTexSubImage2D(t, l, x, y, w, h, f, n, host(c, a.u32()));
}
H(copy_tex_image_2d) {
    GLenum t = a.u32(); GLint l = a.s32(); GLenum ifmt = a.u32(); GLint x = a.s32(), y = a.s32();
    GLsizei w = a.s32(), h = a.s32(); GLint b = a.s32();
    if (!on_screen()) return glCopyTexImage2D(t, l, ifmt, x, y, w, h, b);
    // Copy from the high-resolution screen: the texture gets the internal pixels (sharper effects);
    // texture coordinates are normalised, so the game is unaffected.
    const GLint r[4] = {x, y, w, h};
    GLint ix, iy;
    GLsizei iw, ih;
    map_rect(r, &ix, &iy, &iw, &ih);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, screen_source());
    glCopyTexImage2D(t, l, ifmt, ix, iy, iw, ih, b);
    bind_game_fb();
}
H(generate_mipmap) { p_glGenerateMipmap(a.u32()); }
H(read_pixels) {
    GLint x = a.s32(), y = a.s32(); GLsizei w = a.s32(), h = a.s32(); GLenum f = a.u32(), t = a.u32();
    void* out = const_cast<void*>(host(c, a.u32()));
    const bool native_size = g_cfg.width == g_cfg.logical_w && g_cfg.height == g_cfg.logical_h;
    if (!on_screen() || (native_size && !g_cfg.msaa)) return glReadPixels(x, y, w, h, f, t, out);
    // Downscale the screen into a logical-size buffer so the game gets exactly w*h pixels.
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, screen_source());
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_read_fb);
    p_glBlitFramebuffer(0, 0, g_cfg.width, g_cfg.height, 0, 0, g_cfg.logical_w, g_cfg.logical_h,
                        GL_COLOR_BUFFER_BIT, GL_LINEAR);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_read_fb);
    glReadPixels(x, y, w, h, f, t, out);
    bind_game_fb();
}

// ---- framebuffer objects (OES -> core; enum values are identical) ----------------------------------
H(gen_framebuffers) { GLsizei n = a.s32(); p_glGenFramebuffers(n, static_cast<GLuint*>(const_cast<void*>(host(c, a.u32())))); }
H(gen_renderbuffers) { GLsizei n = a.s32(); p_glGenRenderbuffers(n, static_cast<GLuint*>(const_cast<void*>(host(c, a.u32())))); }
H(delete_framebuffers) { GLsizei n = a.s32(); p_glDeleteFramebuffers(n, static_cast<const GLuint*>(host(c, a.u32()))); }
H(delete_renderbuffers) { GLsizei n = a.s32(); p_glDeleteRenderbuffers(n, static_cast<const GLuint*>(host(c, a.u32()))); }
H(bind_framebuffer) {
    GLenum t = a.u32();
    const bool was_screen = on_screen();
    g_game_fb = a.u32();
    p_glBindFramebuffer(t, g_game_fb ? g_game_fb : g_vfb);  // "0" is our virtual screen
    if (on_screen() != was_screen) {  // screen <-> the game's own target: re-map viewport/scissor
        apply_viewport();
        apply_scissor();
    }
}
H(bind_renderbuffer) { GLenum t = a.u32(); p_glBindRenderbuffer(t, a.u32()); }
H(renderbuffer_storage) { GLenum t = a.u32(), f = a.u32(); GLsizei w = a.s32(), h = a.s32(); p_glRenderbufferStorage(t, f, w, h); }
H(framebuffer_renderbuffer) { GLenum t = a.u32(), at = a.u32(), rt = a.u32(); p_glFramebufferRenderbuffer(t, at, rt, a.u32()); }
H(framebuffer_texture_2d) { GLenum t = a.u32(), at = a.u32(), tt = a.u32(); GLuint tex = a.u32(); p_glFramebufferTexture2D(t, at, tt, tex, a.s32()); }
H(check_framebuffer_status) { c.ret(p_glCheckFramebufferStatus(a.u32())); }

#undef H
#undef M

}  // namespace

bool load_gl() {
    bool ok = true;
#define LOAD(type, name)                                                          \
    p_##name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));             \
    if (!p_##name) { LOG("GL: missing entry point %s", #name); ok = false; }
    GL_EXT_FUNCS(LOAD)
#undef LOAD
    if (ok) tint_init();
    return ok;
}

void register_handlers(rt::Runtime& rt) {
    const std::pair<const char*, cpu::Handler> table[] = {
        {"glEnable", enable}, {"glDisable", disable}, {"glEnableClientState", enable_cs},
        {"glDisableClientState", disable_cs}, {"glAlphaFunc", alpha_func}, {"glBlendFunc", blend_func},
        {"glBlendEquationOES", blend_equation}, {"glColorMask", color_mask}, {"glCullFace", cull_face},
        {"glFrontFace", front_face}, {"glDepthFunc", depth_func}, {"glDepthMask", depth_mask},
        {"glDepthRangef", depth_rangef}, {"glPolygonOffset", polygon_offset}, {"glLineWidth", line_width},
        {"glPointSize", point_size}, {"glPointParameterf", point_param_f}, {"glPointParameterfv", point_param_fv},
        {"glSampleCoverage", sample_coverage}, {"glScissor", scissor}, {"glViewport", viewport},
        {"glShadeModel", shade_model}, {"glHint", hint}, {"glStencilFunc", stencil_func},
        {"glStencilMask", stencil_mask}, {"glStencilOp", stencil_op}, {"glPixelStorei", pixel_storei},
        {"glClear", clear}, {"glClearColor", clear_color}, {"glClearDepthf", clear_depthf},
        {"glClearStencil", clear_stencil}, {"glFlush", flush}, {"glClipPlanef", clip_planef},
        {"glGetError", get_error}, {"glGetString", get_string}, {"glGetIntegerv", get_integerv},
        {"glGetFloatv", get_floatv},
        {"glMatrixMode", matrix_mode}, {"glLoadIdentity", load_identity}, {"glLoadMatrixf", load_matrixf},
        {"glMultMatrixf", mult_matrixf}, {"glPushMatrix", push_matrix}, {"glPopMatrix", pop_matrix},
        {"glColor4f", color4f}, {"glColor4ub", color4ub}, {"glNormal3f", normal3f},
        {"glMultiTexCoord4f", multi_texcoord4f}, {"glFogf", fogf}, {"glFogfv", fogfv}, {"glLightf", lightf},
        {"glLightfv", lightfv}, {"glLightModelf", light_modelf}, {"glLightModelfv", light_modelfv},
        {"glMaterialf", materialf}, {"glMaterialfv", materialfv}, {"glTexEnvf", tex_envf},
        {"glTexEnvi", tex_envi}, {"glTexEnvfv", tex_envfv},
        {"glVertexPointer", vertex_pointer}, {"glTexCoordPointer", texcoord_pointer},
        {"glColorPointer", color_pointer}, {"glNormalPointer", normal_pointer},
        {"glClientActiveTexture", client_active_texture}, {"glDrawArrays", draw_arrays},
        {"glDrawElements", draw_elements},
        {"glGenBuffers", gen_buffers}, {"glDeleteBuffers", delete_buffers}, {"glBindBuffer", bind_buffer},
        {"glBufferData", buffer_data}, {"glBufferSubData", buffer_subdata}, {"glMapBufferOES", map_buffer},
        {"glUnmapBufferOES", unmap_buffer},
        {"glGenTextures", gen_textures}, {"glDeleteTextures", delete_textures}, {"glBindTexture", bind_texture},
        {"glActiveTexture", active_texture}, {"glTexParameteri", tex_parameteri},
        {"glTexParameterf", tex_parameterf}, {"glTexImage2D", tex_image_2d}, {"glTexSubImage2D", tex_subimage_2d},
        {"glCompressedTexImage2D", compressed_tex_image_2d},
        {"glCompressedTexSubImage2D", compressed_tex_subimage_2d}, {"glCopyTexImage2D", copy_tex_image_2d},
        {"glGenerateMipmapOES", generate_mipmap}, {"glReadPixels", read_pixels},
        {"glGenFramebuffersOES", gen_framebuffers}, {"glGenRenderbuffersOES", gen_renderbuffers},
        {"glDeleteFramebuffersOES", delete_framebuffers}, {"glDeleteRenderbuffersOES", delete_renderbuffers},
        {"glBindFramebufferOES", bind_framebuffer}, {"glBindRenderbufferOES", bind_renderbuffer},
        {"glRenderbufferStorageOES", renderbuffer_storage},
        {"glFramebufferRenderbufferOES", framebuffer_renderbuffer},
        {"glFramebufferTexture2DOES", framebuffer_texture_2d},
        {"glCheckFramebufferStatusOES", check_framebuffer_status},
    };
    for (const auto& [name, h] : table) rt.set_handler(name, h);
}

uint64_t draw_calls() { return g_draws; }

bool vfb_init(const VfbConfig& cfg) {
    g_cfg = cfg;
    g_cfg.width = std::max(1, g_cfg.width);
    g_cfg.height = std::max(1, g_cfg.height);
    g_viewport[0] = g_viewport[1] = g_scissor[0] = g_scissor[1] = 0;
    g_viewport[2] = g_scissor[2] = g_cfg.logical_w;
    g_viewport[3] = g_scissor[3] = g_cfg.logical_h;
    return build_vfb();
}

bool vfb_reconfigure(int width, int height, int msaa) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (width == g_cfg.width && height == g_cfg.height && msaa == g_cfg.msaa) return true;
    g_cfg.width = width;
    g_cfg.height = height;
    g_cfg.msaa = msaa;
    if (build_vfb()) return true;
    g_cfg.msaa = 0;
    return build_vfb();
}

void set_texture_observer(TextureObserver obs) { g_tex_observer = std::move(obs); }

void set_world_texture(unsigned tex, bool world) {
    if (world) g_world_tex.insert(tex);
    else g_world_tex.erase(tex);
}

void set_world_tint(float r, float g, float b) {
    const float c[3] = {std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f)};
    if (c[0] != g_tint[0] || c[1] != g_tint[1] || c[2] != g_tint[2]) {
        std::copy(c, c + 3, g_tint);
        g_tint_dirty = true;
    }
}

void replace_compressed_levels(unsigned tex, unsigned format, const std::vector<CompressedLevel>& levels) {
    // Upload through unit 0, then put back what the game had bound there (and its active unit).
    if (g_unit != 0) p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    for (size_t i = 0; i < levels.size(); ++i)
        p_glCompressedTexImage2D(GL_TEXTURE_2D, GLint(i), format, levels[i].w, levels[i].h, 0, levels[i].size,
                                 levels[i].data);
    glBindTexture(GL_TEXTURE_2D, g_bound_2d[0]);
    if (g_unit != 0) p_glActiveTexture(GL_TEXTURE0 + g_unit);
}

void fit_rect(int lw, int lh, int win_w, int win_h, Fit fit, int* x, int* y, int* w, int* h) {
    *w = win_w;
    *h = win_h;
    if (fit == Fit::Aspect || fit == Fit::Integer) {
        double k = std::min(double(win_w) / lw, double(win_h) / lh);
        if (fit == Fit::Integer && k >= 1.0) k = std::floor(k);
        *w = int(lw * k + 0.5);
        *h = int(lh * k + 0.5);
    }
    *x = (win_w - *w) / 2;
    *y = (win_h - *h) / 2;  // from the top
}

void set_anisotropy(int level) {
    GLfloat max_a = 1;
    glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY */, &max_a);
    g_aniso = std::clamp(float(level), 1.0f, std::max(1.0f, max_a));
    reapply_all_filters();
}

void set_texture_filter(TextureFilter f) {
    if (f == g_filter_mode) return;
    g_filter_mode = f;
    reapply_all_filters();
}

const VfbConfig& vfb_config() { return g_cfg; }

void vfb_present(int win_w, int win_h, Fit fit, bool linear, int* rx, int* ry, int* rw, int* rh) {
    // Save the GL state we touch, so the game's (possibly one-time) settings survive.
    GLint vp[4], sb[4];
    GLfloat cc[4];
    GLboolean cm[4], scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_SCISSOR_BOX, sb);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);

    const int sw = g_cfg.width, sh = g_cfg.height;
    int x, y, w, h;
    fit_rect(g_cfg.logical_w, g_cfg.logical_h, win_w, win_h, fit, &x, &y, &w, &h);
    GLuint src = screen_source();
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glBlitFramebuffer(0, 0, sw, sh, x, win_h - y - h, x + w, win_h - y, GL_COLOR_BUFFER_BIT,
                        linear ? GL_LINEAR : GL_NEAREST);
    *rx = x; *ry = y; *rw = w; *rh = h;

    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glScissor(sb[0], sb[1], sb[2], sb[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    // Leave framebuffer 0 (the window) bound for overlay drawing; vfb_restore_binding() rebinds the game's.
}

void vfb_restore_binding() { bind_game_fb(); }

void draw_overlay_rects(int win_w, int win_h, const OverlayRect* rects, int n) {
    GLint sb[4];
    GLfloat cc[4];
    GLboolean cm[4], scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, sb);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (int i = 0; i < n; ++i) {
        const OverlayRect& r = rects[i];
        if (r.w <= 0 || r.h <= 0 || r.x >= win_w || r.y >= win_h) continue;
        glScissor(r.x, win_h - r.y - r.h, r.w, r.h);
        glClearColor(r.r, r.g, r.b, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glScissor(sb[0], sb[1], sb[2], sb[3]);
    if (!scissor) glDisable(GL_SCISSOR_TEST);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
}

bool screenshot(const char* path, int w, int h) {
    GLint read_fb = 0, pack = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    std::vector<uint8_t> px(size_t(w) * h * 4);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glPixelStorei(GL_PACK_ALIGNMENT, pack);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(read_fb));
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!s) return false;
    for (int y = 0; y < h; ++y)  // GL rows are bottom-up
        std::memcpy(static_cast<uint8_t*>(s->pixels) + size_t(y) * s->pitch, &px[size_t(h - 1 - y) * w * 4], size_t(w) * 4);
    bool ok = SDL_SaveBMP(s, path) == 0;
    SDL_FreeSurface(s);
    return ok;
}

void report() { LOG("GL: %llu draw calls so far", (unsigned long long)g_draws); }

}  // namespace gles1
