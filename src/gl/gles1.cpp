#include "gl/gles1.h"

#include <SDL.h>
#include "gl/glapi.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "gl/ffp.h"
#include "gl/glmath.h"
#include "gl/post.h"
#include "gl/s3tc.h"
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

// ---- shadow state: what the game set, readable without a glGet round trip ------------------------
struct Mat4 { float m[16]; };
const Mat4 kIdentity = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
Mat4 mul(const Mat4& a, const Mat4& b) {  // column-major, a * b
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}
// Renderer: false = the game's fixed-function calls go to desktop GL's fixed-function pipeline;
// true = they are emulated in shaders (gl/ffp, the GLES 3 path used on Android).
bool g_shader = false;
GLenum g_client_tex = GL_TEXTURE0;  // glClientActiveTexture
int active_unit();                  // glActiveTexture (defined with the texture state below)

GLenum g_matrix_mode = GL_MODELVIEW;
std::vector<Mat4> g_proj{kIdentity}, g_mv{kIdentity};  // matrix stacks (top = current)
std::vector<Mat4> g_texm[ffp::kUnits] = {{kIdentity}, {kIdentity}, {kIdentity}};  // texture matrices per unit
std::vector<Mat4>* matrix_stack() {
    if (g_matrix_mode == GL_TEXTURE) return active_unit() < ffp::kUnits ? &g_texm[active_unit()] : nullptr;
    return g_matrix_mode == GL_PROJECTION ? &g_proj : g_matrix_mode == GL_MODELVIEW ? &g_mv : nullptr;
}
bool g_fog_on = false, g_depth_test = false, g_lighting = false, g_blend = false, g_depth_write = true;
float g_depth_range_near = 0, g_depth_range_far = 1;  // glDepthRange: near city [0, 0.2], sky and far city [0.2, 1]
GLenum g_blend_dst = GL_ZERO;
bool g_fog_actual = false;  // GL_FOG as GL has it now (the game's call, or fog_before_draw's override)
bool g_stencil_on = false;  // the game's GL_STENCIL_TEST (it never turned it on in the traces)
void track_cap(GLenum cap, bool on) {
    switch (cap) {
        case GL_FOG: g_fog_on = on; g_fog_actual = on; break;  // the game's call sets the real state
        case GL_STENCIL_TEST:
            g_stencil_on = on;
            if (static bool logged = false; on && !logged) {
                logged = true;
                LOG("GL: the game uses the stencil test (the sea's stencil mark, bit 0x80, may clash)");
            }
            break;
        case GL_DEPTH_TEST: g_depth_test = on; break;
        case GL_LIGHTING: g_lighting = on; break;
        case GL_BLEND: g_blend = on; break;
        default: break;
    }
}
// The engine builds left-handed (Irrlicht-style) projections: m[11] = +1. Near/far from the
// GL-style terms m[10] = (f+n)/(f-n), m[14] = -2fn/(f-n); the sky uses an infinite far plane.
bool is_perspective(const Mat4& p) { return std::abs(std::abs(p.m[11]) - 1) < 1e-3f && std::abs(p.m[15]) < 1e-6f; }
float proj_near(const Mat4& p) { return -p.m[14] / (p.m[10] + 1); }
float proj_far(const Mat4& p) { return std::abs(p.m[10] - 1) < 1e-6f ? INFINITY : -p.m[14] / (p.m[10] - 1); }
// A finite 3D projection (the city, people, cars), not the sky's infinite one and not the HUD's ortho.
bool is_world_projection(const Mat4& p) { return is_perspective(p) && std::abs(p.m[10]) > 1.0005f; }
// The sky dome: drawn in the far depth range ([0.2, 1], like the far city) but with the camera's near
// plane (50) instead of the far city's (the streaming radius, thousands).
bool is_sky_draw() { return g_depth_range_near > 0.1f && proj_near(g_proj.back()) < 1000.0f; }

// GANGSTAR_GL_FRAME_TRACE=<frame>: log that frame's structure (clears, targets, projections, and
// runs of draws with the same state), to see where the 3D scene ends and the HUD starts.
int g_frame = 0;
bool frame_tracing() {
    static const int f = [] { const char* e = std::getenv("GANGSTAR_GL_FRAME_TRACE"); return e ? std::atoi(e) : -1; }();
    return g_frame == f;
}
std::string g_trace_sig;
int g_trace_run = 0;
void trace_flush() {
    if (g_trace_run) LOG("frame-trace   %d x draw %s", g_trace_run, g_trace_sig.c_str());
    g_trace_run = 0;
    g_trace_sig.clear();
}
void trace_event(const char* fmt, ...) {
    if (!frame_tracing()) return;
    trace_flush();
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LOG("frame-trace %s", buf);
}
int active_unit();               // defined with the texture state below
bool is_world_tex(GLuint tex);
void trace_draw(GLuint tex0, int count) {
    if (!frame_tracing()) return;
    const Mat4& p = g_proj.back();
    char buf[200];
    if (is_perspective(p)) {
        std::snprintf(buf, sizeof buf, "persp n=%.0f f=%.0f m10=%.6f", proj_near(p), proj_far(p), p.m[10]);
    } else {
        std::snprintf(buf, sizeof buf, "ortho");
    }
    std::string sig = std::string(buf) + (g_fog_on ? " fog" : "") + (g_depth_test ? " ztest" : "") +
                      (g_depth_write ? " zwrite" : "") + (g_lighting ? " lit" : "") +
                      (g_blend ? (g_blend_dst == GL_ONE ? " blend-add" : " blend") : "");
    {  // real GL state, to check the shadows and see how each kind of object is drawn
        GLint env = 0;
        p_glActiveTexture(GL_TEXTURE0);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env);
        p_glActiveTexture(GL_TEXTURE0 + active_unit());
        char more[160];
        std::snprintf(more, sizeof more, " | gl:%s%s%s%s env0=%#x tex0=%u%s n=%d", glIsEnabled(GL_LIGHTING) ? " LIT" : "",
                      glIsEnabled(GL_COLOR_ARRAY) ? " colarr" : "", glIsEnabled(GL_NORMAL_ARRAY) ? " normals" : "",
                      glIsEnabled(GL_COLOR_MATERIAL) ? " colmat" : "", unsigned(env), tex0, is_world_tex(tex0) ? " world" : "", count);
        sig += more;
    }
    if (sig != g_trace_sig) {
        trace_flush();
        g_trace_sig = sig;
    }
    ++g_trace_run;
}

#define H(fn) void fn(Cpu& c, Args& a)
#define M (c.rt().mem())

const void* host(Cpu& c, uint32_t p) { return p ? M.ptr(p) : nullptr; }
// Vertex-array / index pointers: offsets when a buffer object is bound, guest pointers otherwise.
const void* array_ptr(Cpu& c, uint32_t p, bool bound) {
    return bound ? reinterpret_cast<const void*>(uintptr_t(p)) : host(c, p);
}

// ---- state -----------------------------------------------------------------------------------------
void set_cap(GLenum cap, bool on) {
    track_cap(cap, on);
    if (g_shader && cap == GL_FOG) ffp::set_fog_override(0);  // the game's own fog call wins (as in GL)
    if (g_shader && ffp::is_ffp_cap(cap)) ffp::enable(cap, on, active_unit());
    else if (on) glEnable(cap);
    else glDisable(cap);
}
H(enable) { set_cap(a.u32(), true); }
H(disable) { set_cap(a.u32(), false); }
bool g_color_array_on = false;           // GL_COLOR_ARRAY enabled (glow colours)
float g_cur_color[4] = {1, 1, 1, 1};     // glColor (glow colours when no colour array)
H(enable_cs) {
    GLenum arr = a.u32();
    if (arr == GL_COLOR_ARRAY) g_color_array_on = true;
    if (g_shader) ffp::client_state(arr, true, int(g_client_tex - GL_TEXTURE0));
    else glEnableClientState(arr);
}
H(disable_cs) {
    GLenum arr = a.u32();
    if (arr == GL_COLOR_ARRAY) g_color_array_on = false;
    if (g_shader) ffp::client_state(arr, false, int(g_client_tex - GL_TEXTURE0));
    else glDisableClientState(arr);
}
H(alpha_func) {
    GLenum f = a.u32(); float r = a.f32();
    if (g_shader) ffp::alpha_func(f, r);
    else glAlphaFunc(f, r);
}
H(blend_func) { GLenum s = a.u32(), d = a.u32(); g_blend_dst = d; glBlendFunc(s, d); }
H(blend_equation) { p_glBlendEquation(a.u32()); }
H(color_mask) { GLboolean r = GLboolean(a.u32()), g = GLboolean(a.u32()), b = GLboolean(a.u32()), al = GLboolean(a.u32()); glColorMask(r, g, b, al); }
H(cull_face) { glCullFace(a.u32()); }
H(front_face) { glFrontFace(a.u32()); }
H(depth_func) { glDepthFunc(a.u32()); }
H(depth_mask) { GLboolean m = GLboolean(a.u32()); g_depth_write = m != 0; glDepthMask(m); }
H(depth_rangef) {
    float n = a.f32(), f = a.f32();
    trace_event("depth range %.3f %.3f", n, f);
    g_depth_range_near = n;
    g_depth_range_far = f;
    glDepthRange(n, f);
}
H(polygon_offset) { float f = a.f32(), u = a.f32(); glPolygonOffset(f, u); }
H(line_width) { glLineWidth(float(a.f32() * ky())); }
H(point_size) {
    const float s = float(a.f32() * ky());
    if (g_shader) ffp::point_size(s);
    else glPointSize(s);
}
H(point_param_f) {  // size limits / attenuation: not emulated in shader mode (fixed sizes)
    GLenum p = a.u32(); float v = a.f32();
    if (p == GL_POINT_SIZE_MIN || p == GL_POINT_SIZE_MAX) v = float(v * ky());
    if (!g_shader) p_glPointParameterf(p, v);
}
H(point_param_fv) {
    GLenum p = a.u32(); auto v = static_cast<const GLfloat*>(host(c, a.u32()));
    if (!g_shader) p_glPointParameterfv(p, v);
}
H(sample_coverage) { float v = a.f32(); GLboolean inv = GLboolean(a.u32()); p_glSampleCoverage(v, inv); }
H(scissor) { for (GLint& v : g_scissor) v = a.s32(); apply_scissor(); }
H(viewport) { for (GLint& v : g_viewport) v = a.s32(); apply_viewport(); }
H(shade_model) {
    GLenum m = a.u32();
    if (!g_shader) glShadeModel(m);  // shader mode: always smooth (the game never asks for flat)
}
H(hint) {
    GLenum t = a.u32(), m = a.u32();
    if (!g_shader || t == GL_GENERATE_MIPMAP_HINT) glHint(t, m);  // the only GLES 1 hint GLES 3 still has
}
// the game's stencil state (the sea's draws change it and put it back)
GLenum g_st_func = GL_ALWAYS, g_st_op[3] = {GL_KEEP, GL_KEEP, GL_KEEP};
GLint g_st_ref = 0;
GLuint g_st_vmask = 0xFFFFFFFFu, g_st_wmask = 0xFFFFFFFFu;
H(stencil_func) { g_st_func = a.u32(); g_st_ref = a.s32(); g_st_vmask = a.u32(); glStencilFunc(g_st_func, g_st_ref, g_st_vmask); }
H(stencil_mask) { g_st_wmask = a.u32(); glStencilMask(g_st_wmask); }
H(stencil_op) { for (GLenum& o : g_st_op) o = a.u32(); glStencilOp(g_st_op[0], g_st_op[1], g_st_op[2]); }
H(pixel_storei) { GLenum p = a.u32(); GLint v = a.s32(); glPixelStorei(p, v); }
H(clear) {
    GLbitfield m = a.u32();
    trace_event("clear%s%s%s (fb %u)", m & GL_COLOR_BUFFER_BIT ? " color" : "", m & GL_DEPTH_BUFFER_BIT ? " depth" : "",
                m & GL_STENCIL_BUFFER_BIT ? " stencil" : "", g_game_fb);
    // the sea's stencil mark lives for one frame: cleared with the depth (the game never clears the stencil)
    if ((m & GL_DEPTH_BUFFER_BIT) && !(m & GL_STENCIL_BUFFER_BIT) && !g_stencil_on) {
        m |= GL_STENCIL_BUFFER_BIT;
        if (g_st_wmask != 0xFFFFFFFFu) glStencilMask(0xFF);
        glClear(m);
        if (g_st_wmask != 0xFFFFFFFFu) glStencilMask(g_st_wmask);
        return;
    }
    glClear(m);
}
H(clear_color) { float r = a.f32(), g = a.f32(), b = a.f32(), al = a.f32(); glClearColor(r, g, b, al); }
H(clear_depthf) { glClearDepth(a.f32()); }
H(clear_stencil) { glClearStencil(a.s32()); }
H(flush) { glFlush(); }
using glmath::invert4;
H(clip_planef) {
    GLenum p = a.u32();
    const float* e = static_cast<const float*>(host(c, a.u32()));
    if (g_shader) {
        // GL stores the plane in eye space: plane x inverse(modelview at the time of the call).
        float inv[16];
        if (!invert4(g_mv.back().m, inv)) return;
        float eye[4];
        for (int i = 0; i < 4; ++i) eye[i] = e[0] * inv[i * 4] + e[1] * inv[i * 4 + 1] + e[2] * inv[i * 4 + 2] + e[3] * inv[i * 4 + 3];
        ffp::clip_plane(int(p - GL_CLIP_PLANE0), eye);
        return;
    }
#ifndef GANGSTAR_GLES  // (the GLES build always runs the shader renderer)
    const double d[4] = {e[0], e[1], e[2], e[3]};
    glClipPlane(p, d);
#endif
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
    if (p == GL_MAX_TEXTURE_UNITS && g_shader) { *out = ffp::kUnits; return; }
    if (p == GL_MATRIX_MODE) { *out = GLint(g_matrix_mode); return; }
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
    // matrices from the shadow stacks (shader mode has none in GL)
    if (p == GL_MODELVIEW_MATRIX || p == GL_PROJECTION_MATRIX || p == GL_TEXTURE_MATRIX) {
        const std::vector<Mat4>& s = p == GL_MODELVIEW_MATRIX ? g_mv : p == GL_PROJECTION_MATRIX ? g_proj
                                   : g_texm[std::min(active_unit(), ffp::kUnits - 1)];
        std::memcpy(out, s.back().m, sizeof(Mat4));
        return;
    }
    glGetFloatv(p, out);
}

// ---- fixed-function pipeline ---------------------------------------------------------------------
// Matrices live in the shadow stacks; shader mode passes them per draw instead of to GL.
H(matrix_mode) { g_matrix_mode = a.u32(); if (!g_shader) glMatrixMode(g_matrix_mode); }
H(load_identity) {
    if (auto* s = matrix_stack()) s->back() = kIdentity;
    if (!g_shader) glLoadIdentity();
}
H(load_matrixf) {
    const GLfloat* m = static_cast<const GLfloat*>(host(c, a.u32()));
    if (auto* s = matrix_stack()) std::memcpy(s->back().m, m, sizeof(Mat4));
    if (!g_shader) glLoadMatrixf(m);
}
H(mult_matrixf) {
    const GLfloat* m = static_cast<const GLfloat*>(host(c, a.u32()));
    if (auto* s = matrix_stack()) {
        Mat4 b;
        std::memcpy(b.m, m, sizeof b);
        s->back() = mul(s->back(), b);
    }
    if (!g_shader) glMultMatrixf(m);
}
H(push_matrix) {
    if (auto* s = matrix_stack()) s->push_back(s->back());
    if (!g_shader) glPushMatrix();
}
H(pop_matrix) {
    if (auto* s = matrix_stack(); s && s->size() > 1) s->pop_back();
    if (!g_shader) glPopMatrix();
}
H(color4f) {
    float r = a.f32(), g = a.f32(), b = a.f32(), al = a.f32();
    g_cur_color[0] = r, g_cur_color[1] = g, g_cur_color[2] = b, g_cur_color[3] = al;
    if (g_shader) ffp::color(r, g, b, al);
    else glColor4f(r, g, b, al);
}
H(color4ub) {
    GLubyte r = GLubyte(a.u32()), g = GLubyte(a.u32()), b = GLubyte(a.u32()), al = GLubyte(a.u32());
    g_cur_color[0] = r / 255.0f, g_cur_color[1] = g / 255.0f, g_cur_color[2] = b / 255.0f, g_cur_color[3] = al / 255.0f;
    if (g_shader) ffp::color(r / 255.0f, g / 255.0f, b / 255.0f, al / 255.0f);
    else glColor4ub(r, g, b, al);
}
H(normal3f) { float x = a.f32(), y = a.f32(), z = a.f32(); if (!g_shader) glNormal3f(x, y, z); }
H(multi_texcoord4f) {
    GLenum t = a.u32(); float s = a.f32(), u = a.f32(), r = a.f32(), q = a.f32();
    if (g_shader) ffp::multi_texcoord(int(t - GL_TEXTURE0), s, u, r, q);
    else p_glMultiTexCoord4f(t, s, u, r, q);
}
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
// ---- atmospheric fog (see gles1.h) ----------------------------------------------------------------
// The game's fog is linear from the texture streaming radius to past the far city's end, so the
// detailed city (which ends before the fog starts) is never fogged. Atmospheric mode keeps the
// game's time-of-day fog colour and replaces the distances with exponential-squared haze by eye
// distance (radial where the driver supports NV_fog_distance), almost opaque at the far city's end.
struct GameFog { GLint mode = GL_EXP; float start = 0, end = 1, density = 1; };
GameFog g_game_fog;           // the distances the game asked for (restored when atmospheric is off)
FogConfig g_fog_cfg;
bool g_fog_radial = false;    // GL_NV_fog_distance available
constexpr GLenum kFogDistanceMode = 0x855A, kEyeRadial = 0x855B, kEyePlaneAbsolute = 0x855C;

// ---- screen effects (post.h): what the scene used, and when it ends -----------------------------
post::Scene g_post_scene;
bool g_post_pending = false;   // a 3D scene was drawn on screen this frame and has not been processed
bool g_post_view_seen = false; // the camera view matrix was taken from a city draw this frame
float g_cam_view[16];          // the engine camera's view (set_camera_view)
float g_cam_inv[16];           // and its inverse (eye -> world)
bool g_cam_view_set = false;
float g_fog_rgb[3] = {0.6f, 0.65f, 0.7f};      // the game's fog colour (time of day)
float g_fog_col_actual[3] = {-1, -1, -1};       // the colour GL has (the game's, or the late fog's)
float g_fog_density_now = 0;

// One fog parameter to whichever pipeline is in use.
void fog_param(GLenum p, float v) {
    if (g_shader) ffp::fog(p, &v, 1);
    else glFogf(p, v);
}
void apply_fog() {
    if (g_fog_cfg.atmospheric && g_fog_cfg.screen) {
        g_fog_density_now = post::late_fog().density;  // only for the draws after the screen pass
        fog_param(GL_FOG_MODE, float(GL_EXP2));
        fog_param(GL_FOG_DENSITY, g_fog_density_now);
    } else if (g_fog_cfg.atmospheric) {
        // exp(-(d*z)^2) = 3% at `far`: d*far = sqrt(-ln 0.03) = 1.873
        const float d = g_fog_cfg.strength <= 0 ? 0.0f : 1.873f * g_fog_cfg.strength / std::max(1.0f, g_fog_cfg.far_end);
        g_fog_density_now = d;
        fog_param(GL_FOG_MODE, float(GL_EXP2));
        fog_param(GL_FOG_DENSITY, d);
    } else {
        g_fog_density_now = 0;
        fog_param(GL_FOG_MODE, float(g_game_fog.mode));
        fog_param(GL_FOG_START, g_game_fog.start);
        fog_param(GL_FOG_END, g_game_fog.end);
        fog_param(GL_FOG_DENSITY, g_game_fog.density);
    }
    // radial distance: the shader can always do it; desktop GL needs NV_fog_distance
    if (g_shader || g_fog_radial) fog_param(kFogDistanceMode, float(g_fog_cfg.atmospheric ? kEyeRadial : kEyePlaneAbsolute));
}
// Records a distance parameter; true if atmospheric fog owns it (do not pass it to GL).
bool game_fog_param(GLenum p, float v) {
    switch (p) {
        case GL_FOG_MODE: g_game_fog.mode = GLint(v); break;
        case GL_FOG_START: g_game_fog.start = v; break;
        case GL_FOG_END: g_game_fog.end = v; break;
        case GL_FOG_DENSITY: g_game_fog.density = v; break;
        default: return false;
    }
    return g_fog_cfg.atmospheric;
}
bool is_world_tex(GLuint tex);
// Screen effects run on the finished 3D scene: at the first 2D (orthographic) draw on screen after
// it, else at present. 3D draws record the projections of the two depth ranges and, from a city
// batch (world texture, drawn in world space), the camera's view matrix.
bool g_post_split = false;     // the atmosphere stage ran this frame (just before the first glow)
int g_glow_draws = 0;         // glow draws since the last atmosphere stage (a car light's quads share one)
void run_post(post::Stage stage) {
    g_post_pending = false;
    if (!post::active()) return;
    post::Scene& s = g_post_scene;
    s.fbo = g_vfb;
    s.w = g_cfg.width;
    s.h = g_cfg.height;
    std::memcpy(s.fog_color, g_fog_rgb, sizeof s.fog_color);
    s.fog_density = g_fog_density_now;
    trace_event(stage == post::Stage::Atmosphere ? "screen effects: atmosphere"
                : stage == post::Stage::Finish   ? "screen effects: finish"
                                                 : "screen effects");
    post::run(s, !g_shader, stage);
    if (g_shader) ffp::invalidate();
    if (stage != post::Stage::Finish) {  // the lights took the glows recorded since the last time
        s.glow_count = 0;
        g_glow_draws = 0;
    }
}
// End of the 3D scene: the finish, or everything when no glow split the frame.
void run_post() { run_post(g_post_split ? post::Stage::Finish : post::Stage::All); }
void post_before_draw(GLuint tex0) {
    if (!on_screen()) return;
    const Mat4& p = g_proj.back();
    if (!is_perspective(p)) {
        if (g_post_pending) run_post();
        return;
    }
    g_post_pending = true;
    if (!is_world_projection(p)) return;
    // The first glow (additive) after the solid city and the sea: the atmosphere stage runs now, so
    // lamp, neon and headlight glows are drawn over the fogged, lit scene instead of being fogged by it.
    if (!g_post_split && g_post_view_seen && g_blend && g_blend_dst == GL_ONE && !is_sky_draw() && post::active()) {
        run_post(post::Stage::Atmosphere);
        g_post_split = true;
        g_post_pending = true;  // the glows and the rest of the scene are still to come
    }
    post::Scene& s = g_post_scene;
    if (g_depth_range_far < 0.999f) {
        s.split = g_depth_range_far;
        s.near_n = proj_near(p);
        s.near_f = proj_far(p);
        s.tan_half_x = 1.0f / p.m[0];
        s.tan_half_y = 1.0f / p.m[5];
        // City batches: normals, no colour array (people / cars / props have colour arrays), drawn in
        // world space, so their modelview is the camera's view.
        const bool normals = g_shader ? ffp::array_enabled(GL_NORMAL_ARRAY) : glIsEnabled(GL_NORMAL_ARRAY) != 0;
        const bool colors = g_shader ? ffp::array_enabled(GL_COLOR_ARRAY) : glIsEnabled(GL_COLOR_ARRAY) != 0;
        if (!g_post_view_seen && g_cam_view_set) {
            std::memcpy(s.view, g_cam_view, sizeof s.view);
            s.view_valid = g_post_view_seen = true;
        } else if (!g_post_view_seen && (is_world_tex(tex0) || (normals && !colors))) {
            std::memcpy(s.view, g_mv.back().m, sizeof s.view);
            s.view_valid = g_post_view_seen = true;
        }
        // GANGSTAR_CAM_TRACE=1: how often a city-looking draw's modelview differs from the camera
        static const bool cam_trace = std::getenv("GANGSTAR_CAM_TRACE") != nullptr;
        if (cam_trace && g_cam_view_set && (is_world_tex(tex0) || (normals && !colors))) {
            float diff = 0;
            for (int i = 0; i < 16; ++i) diff = std::max(diff, std::abs(g_mv.back().m[i] - g_cam_view[i]) / (i >= 12 ? 100.0f : 1.0f));
            static int n = 0;
            if (diff > 0.01f && n++ < 40)
                LOG("cam-trace f%d: draw tex %u %s modelview differs from the camera by %.3f (mv t %.0f %.0f %.0f, cam t %.0f %.0f %.0f)",
                    g_frame, tex0, is_world_tex(tex0) ? "world" : "normals", diff, g_mv.back().m[12], g_mv.back().m[13],
                    g_mv.back().m[14], g_cam_view[12], g_cam_view[13], g_cam_view[14]);
        }
    } else if (!is_sky_draw()) {
        s.far_n = proj_near(p);
        s.far_f = proj_far(p);
    }
}

// People and cars are drawn without fog (they were always close on the phone); with atmospheric
// fog every opaque 3D draw is fogged, so a far car sits in the haze like the street around it.
void fog_before_draw() {
    // Fog the draws the game leaves unfogged: people, cars and props, and the alpha-blended ones such
    // as the sea (texture "water", drawn blended in both depth ranges, never fogged by the game, so
    // it stayed clear while the city behind it faded). Not additive glows (fog would brighten them)
    // and not the sky dome (its own projection, near plane 50, in the far depth range).
    const Mat4& pr = g_proj.back();
    const bool sky = g_depth_range_far >= 0.999f && proj_near(pr) < 1000.0f;
    const bool additive = g_blend && g_blend_dst == GL_ONE;
    bool want;
    if (g_fog_cfg.atmospheric && g_fog_cfg.screen) {
        // The screen pass fogs the solid scene and the sea; what is drawn after it (glows, decals) gets
        // the same haze by eye distance here, glows fading to black (their colour is light).
        const post::LateFog late = post::late_fog();
        want = g_post_split && late.density > 0 && g_depth_test && !sky && is_world_projection(pr);
        if (want) {
            static const float black[3] = {0, 0, 0};
            const float* col = additive ? black : late.color;
            if (std::memcmp(col, g_fog_col_actual, sizeof g_fog_col_actual) != 0) {
                const float v[4] = {col[0], col[1], col[2], 1.0f};
                if (g_shader) ffp::fog(GL_FOG_COLOR, v, 4);
                else glFogfv(GL_FOG_COLOR, v);
                std::memcpy(g_fog_col_actual, col, sizeof g_fog_col_actual);
            }
            if (late.density != g_fog_density_now) {
                g_fog_density_now = late.density;
                fog_param(GL_FOG_DENSITY, late.density);
            }
        }
    } else {
        want = g_fog_on || (g_fog_cfg.atmospheric && g_fog_cfg.fog_everything && !additive && g_depth_test && !sky &&
                            is_world_projection(pr));
    }
    if (want == g_fog_actual) return;
    if (g_shader) ffp::set_fog_override(want ? 1 : -1);
    else if (want) glEnable(GL_FOG);
    else glDisable(GL_FOG);
    g_fog_actual = want;
}

H(fogf) {
    GLenum p = a.u32(); float v = a.f32();
    light_trace("glFogf", p, 0, &v, 1);
    if (!game_fog_param(p, v)) fog_param(p, v);
}
H(fogfv) {
    GLenum p = a.u32(); auto v = static_cast<const GLfloat*>(host(c, a.u32()));
    light_trace("glFogfv", p, 0, v, param_count(p));
    if (p == GL_FOG_COLOR) {
        std::memcpy(g_fog_rgb, v, sizeof g_fog_rgb);
        std::memcpy(g_fog_col_actual, v, sizeof g_fog_col_actual);
    }
    if (param_count(p) != 1 || !game_fog_param(p, v[0])) {
        if (g_shader) ffp::fog(p, v, param_count(p));
        else glFogfv(p, v);
    }
}
// Lighting: the game sets lights and materials but never enables GL_LIGHTING; shader mode ignores them.
H(lightf) { GLenum l = a.u32(), p = a.u32(); float v = a.f32(); light_trace("glLightf", l, p, &v, 1); if (!g_shader) glLightf(l, p, v); }
H(lightfv) { GLenum l = a.u32(), p = a.u32(); auto v = static_cast<const GLfloat*>(host(c, a.u32())); light_trace("glLightfv", l, p, v, param_count(p)); if (!g_shader) glLightfv(l, p, v); }
H(light_modelf) { GLenum p = a.u32(); float v = a.f32(); light_trace("glLightModelf", p, 0, &v, 1); if (!g_shader) glLightModelf(p, v); }
H(light_modelfv) { GLenum p = a.u32(); auto v = static_cast<const GLfloat*>(host(c, a.u32())); light_trace("glLightModelfv", p, 0, v, param_count(p)); if (!g_shader) glLightModelfv(p, v); }
H(materialf) { GLenum f = a.u32(), p = a.u32(); float v = a.f32(); light_trace("glMaterialf", f, p, &v, 1); if (!g_shader) glMaterialf(f, p, v); }
H(materialfv) { GLenum f = a.u32(), p = a.u32(); auto v = static_cast<const GLfloat*>(host(c, a.u32())); light_trace("glMaterialfv", f, p, v, param_count(p)); if (!g_shader) glMaterialfv(f, p, v); }
H(tex_envf) {
    GLenum t = a.u32(), p = a.u32(); float v = a.f32();
    light_trace("glTexEnvf", t, p, &v, 1);
    if (g_shader) ffp::tex_env(active_unit(), t, p, &v, 1);
    else glTexEnvf(t, p, v);
}
H(tex_envi) {
    GLenum t = a.u32(), p = a.u32(); GLint v = a.s32(); float fv = float(v);
    light_trace("glTexEnvi", t, p, &fv, 1);
    if (g_shader) ffp::tex_env(active_unit(), t, p, &fv, 1);
    else glTexEnvi(t, p, v);
}
H(tex_envfv) {
    GLenum t = a.u32(), p = a.u32(); auto v = static_cast<const GLfloat*>(host(c, a.u32()));
    light_trace("glTexEnvfv", t, p, v, param_count(p));
    if (g_shader) ffp::tex_env(active_unit(), t, p, v, param_count(p));
    else glTexEnvfv(t, p, v);
}

// ---- vertex arrays and drawing ---------------------------------------------------------------------
// Frame trace of 2D draws: the client arrays (guest pointers) and texture sizes, to print each draw's
// vertex / texture coordinate bounds (debugging HUD sprites, e.g. the clipped radio box).
struct ArrayDbg { GLint size = 0; GLenum type = 0; GLsizei stride = 0; uint32_t ptr = 0; GLuint buf = 0; };
ArrayDbg g_dbg_vtx, g_dbg_tc, g_dbg_col;
std::unordered_map<GLuint, std::pair<int, int>> g_dbg_tex_size;
float dbg_component(Cpu& c, const ArrayDbg& d, uint32_t i, int k) {
    const int sz = d.type == GL_FLOAT || d.type == GL_FIXED ? 4 : d.type == GL_SHORT ? 2 : 1;
    const uint32_t a = d.ptr + i * (d.stride ? d.stride : d.size * sz) + k * sz;
    switch (d.type) {
        case GL_FLOAT: { float f; std::memcpy(&f, M.ptr(a), 4); return f; }
        case GL_FIXED: return int32_t(M.r32(a)) / 65536.0f;
        case GL_SHORT: return float(int16_t(M.r16(a)));
        case GL_BYTE: return float(int8_t(M.r8(a)));
        default: return float(M.r8(a));
    }
}
void trace_geometry(Cpu& c, GLint first, GLsizei n, GLenum itype, uint32_t iptr, GLuint tex) {
    if (!frame_tracing() || g_dbg_vtx.buf || !g_dbg_vtx.ptr || n <= 0) return;
    if (is_perspective(g_proj.back()) && !g_blend) return;  // 2D draws and blended 3D draws (glows)
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
    const bool uv = g_dbg_tc.ptr && !g_dbg_tc.buf;
    for (GLsizei j = 0; j < n; ++j) {
        uint32_t i = uint32_t(first + j);
        if (iptr) i = itype == GL_UNSIGNED_SHORT ? M.r16(iptr + 2 * j) : itype == GL_UNSIGNED_BYTE ? M.r8(iptr + j) : M.r32(iptr + 4 * j);
        const float x = dbg_component(c, g_dbg_vtx, i, 0), y = dbg_component(c, g_dbg_vtx, i, 1);
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
        if (uv) {
            const float u = dbg_component(c, g_dbg_tc, i, 0), v = dbg_component(c, g_dbg_tc, i, 1);
            u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
        }
    }
    const Mat4& mv = g_mv.back();
    GLint wrap_s = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap_s);
    const auto ts = g_dbg_tex_size.count(tex) ? g_dbg_tex_size[tex] : std::pair<int, int>{0, 0};
    trace_flush();
    LOG("frame-trace     2d n=%d tex %u (%dx%d wrap %#x) xy %.1f..%.1f x %.1f..%.1f mv scale %g %g move %.1f %.1f uv %.3f..%.3f x %.3f..%.3f",
        n, tex, ts.first, ts.second, unsigned(wrap_s), x0, x1, y0, y1, mv.m[0], mv.m[5], mv.m[12], mv.m[13], u0, u1, v0, v1);
#ifndef GANGSTAR_GLES  // GLES has no texture read-back (debugging aid on desktop only)
    static std::unordered_map<GLuint, bool> dumped;  // save each traced 2D texture once (TGA, next to the log)
    if (tex && !dumped[tex]) {
        dumped[tex] = true;
        GLint w = 0, h = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
            std::vector<uint8_t> px(size_t(w) * h * 4);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
            const char* home = std::getenv("GANGSTAR_HOME");
            const std::string path = std::string(home ? home : ".") + "/tex_" + std::to_string(tex) + ".tga";
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                const uint8_t hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(w), uint8_t(w >> 8), uint8_t(h), uint8_t(h >> 8), 32, 8};
                std::fwrite(hdr, 1, 18, f);
                std::fwrite(px.data(), 1, px.size(), f);
                std::fclose(f);
                LOG("frame-trace     texture %u is %dx%d -> %s", tex, w, h, path.c_str());
            }
        }
    }
#endif
    if (!iptr || n % 6) return;
    for (GLsizei q = 0; q < n; q += 6) {  // indexed quads (two triangles each)
        float qx0 = 1e9f, qx1 = -1e9f, qy0 = 1e9f, qy1 = -1e9f, qu0 = 1e9f, qu1 = -1e9f, qv0 = 1e9f, qv1 = -1e9f;
        for (int j = 0; j < 6; ++j) {
            const uint32_t i = itype == GL_UNSIGNED_SHORT ? M.r16(iptr + 2 * (q + j)) : M.r8(iptr + q + j);
            const float x = dbg_component(c, g_dbg_vtx, i, 0), y = dbg_component(c, g_dbg_vtx, i, 1);
            qx0 = std::min(qx0, x), qx1 = std::max(qx1, x), qy0 = std::min(qy0, y), qy1 = std::max(qy1, y);
            if (uv) {
                const float u = dbg_component(c, g_dbg_tc, i, 0), v = dbg_component(c, g_dbg_tc, i, 1);
                qu0 = std::min(qu0, u), qu1 = std::max(qu1, u), qv0 = std::min(qv0, v), qv1 = std::max(qv1, v);
            }
        }
        LOG("frame-trace       quad xy %.1f..%.1f x %.1f..%.1f uv %.4f..%.4f x %.4f..%.4f", qx0, qx1, qy0, qy1, qu0, qu1, qv0, qv1);
    }
}
// Dynamic lights (post.h): each additive 3D draw on screen is glow sprites from the light-effects
// atlas (lamps, neon, headlight / taillight / police flares, headlight beams). Every quad is recorded
// with its centre (eye space, through its own modelview), size, atlas rectangle (after the texture
// matrix) and whether it is a city billboard (drawn with the camera view) or a car's light (a model
// matrix; the model origin is the light's mount point). Client arrays only.
// (g_glow_draws: with the post state above)
GLuint bound_tex0();
void glow_capture(Cpu& c, GLenum mode, GLint first, GLsizei n, GLenum itype, uint32_t iptr) {
    if (!on_screen() || !g_blend || g_blend_dst != GL_ONE || !is_world_projection(g_proj.back()) || g_dbg_vtx.buf ||
        !g_dbg_vtx.ptr || n <= 0 || !g_cam_view_set)
        return;
    post::Scene& s = g_post_scene;
    const Mat4& mv = g_mv.back();
    const Mat4& tm = g_texm[0].back();
    const bool uv_ok = g_dbg_tc.ptr && !g_dbg_tc.buf;
    // city billboards are drawn with the camera's view as their modelview
    const float* view = g_cam_view_set ? g_cam_view : s.view;
    const bool world = (g_cam_view_set || s.view_valid) && std::fabs(mv.m[12] - view[12]) + std::fabs(mv.m[13] - view[13]) +
                                                               std::fabs(mv.m[14] - view[14]) < 1.0f;
    const int draw = g_glow_draws++;
    const GLsizei group = mode == GL_TRIANGLES ? 6 : n;
    for (GLsizei q = 0; q + group <= n && s.glow_count < post::Scene::kMaxGlows; q += group) {
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        float uv[4] = {1e30f, 1e30f, -1e30f, -1e30f};
        for (GLsizei j = 0; j < group; ++j) {
            uint32_t i = uint32_t(first + q + j);
            if (iptr) i = itype == GL_UNSIGNED_SHORT ? M.r16(iptr + 2 * (q + j)) : itype == GL_UNSIGNED_BYTE ? M.r8(iptr + q + j) : M.r32(iptr + 4 * (q + j));
            for (int k = 0; k < 3; ++k) {
                const float v = k < g_dbg_vtx.size ? dbg_component(c, g_dbg_vtx, i, k) : 0.0f;
                lo[k] = std::min(lo[k], v);
                hi[k] = std::max(hi[k], v);
            }
            if (uv_ok) {  // GLES 1 does not normalise integer texture coordinates: the texture matrix scales them
                const float u0 = dbg_component(c, g_dbg_tc, i, 0), v0 = dbg_component(c, g_dbg_tc, i, 1);
                const float u = tm.m[0] * u0 + tm.m[4] * v0 + tm.m[12], v = tm.m[1] * u0 + tm.m[5] * v0 + tm.m[13];
                uv[0] = std::min(uv[0], u), uv[1] = std::min(uv[1], v), uv[2] = std::max(uv[2], u), uv[3] = std::max(uv[3], v);
            }
        }
        const float o[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
        post::Scene::Glow& g = s.glows[s.glow_count];
        float eye[3];
        for (int k = 0; k < 3; ++k) eye[k] = mv.m[k] * o[0] + mv.m[4 + k] * o[1] + mv.m[8 + k] * o[2] + mv.m[12 + k];
        for (int k = 0; k < 3; ++k) {  // to world with the camera of this frame
            g.pos[k] = g_cam_inv[k] * eye[0] + g_cam_inv[4 + k] * eye[1] + g_cam_inv[8 + k] * eye[2] + g_cam_inv[12 + k];
            g.origin[k] = g_cam_inv[k] * mv.m[12] + g_cam_inv[4 + k] * mv.m[13] + g_cam_inv[8 + k] * mv.m[14] + g_cam_inv[12 + k];
        }
        // size in world units: the model matrix may scale (car lights)
        const float scale = std::sqrt(mv.m[0] * mv.m[0] + mv.m[1] * mv.m[1] + mv.m[2] * mv.m[2]);
        g.size = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}) * (scale > 0 ? scale : 1.0f);
        if (!uv_ok) uv[0] = uv[1] = 0, uv[2] = uv[3] = 1;
        std::copy(uv, uv + 4, g.uv);
        g.tex = bound_tex0();
        g.world = world;
        g.draw = draw;
        if (frame_tracing() && q < 6 * 3)
            LOG("frame-trace glow tex %u draw %d n %d: at %.0f %.0f %.0f size %.0f uv %.3f..%.3f x %.3f..%.3f origin %.0f %.0f %.0f %s",
                g.tex, draw, int(n), g.pos[0], g.pos[1], g.pos[2], g.size, uv[0], uv[2], uv[1], uv[3], g.origin[0], g.origin[1],
                g.origin[2], world ? "city" : "model");
        ++s.glow_count;
    }
}

H(vertex_pointer) {
    GLint s = a.s32(); GLenum t = a.u32(); GLsizei st = a.s32(); uint32_t p = a.u32();
    g_dbg_vtx = {s, t, st, p, g_array_buffer};
    if (g_shader) ffp::vertex_pointer({s, t, st, array_ptr(c, p, g_array_buffer), g_array_buffer});
    else glVertexPointer(s, t, st, array_ptr(c, p, g_array_buffer));
}
H(texcoord_pointer) {
    GLint s = a.s32(); GLenum t = a.u32(); GLsizei st = a.s32(); uint32_t p = a.u32();
    if (g_client_tex == GL_TEXTURE0) g_dbg_tc = {s, t, st, p, g_array_buffer};
    if (g_shader) ffp::texcoord_pointer(int(g_client_tex - GL_TEXTURE0), {s, t, st, array_ptr(c, p, g_array_buffer), g_array_buffer});
    else glTexCoordPointer(s, t, st, array_ptr(c, p, g_array_buffer));
}
H(color_pointer) {
    GLint s = a.s32(); GLenum t = a.u32(); GLsizei st = a.s32(); uint32_t p = a.u32();
    g_dbg_col = {s, t, st, p, g_array_buffer};
    if (g_shader) ffp::color_pointer({s, t, st, array_ptr(c, p, g_array_buffer), g_array_buffer});
    else glColorPointer(s, t, st, array_ptr(c, p, g_array_buffer));
}
H(normal_pointer) {  // normals only matter for GL lighting, which the game never turns on
    GLenum t = a.u32(); GLsizei st = a.s32(); uint32_t p = a.u32();
    if (!g_shader) glNormalPointer(t, st, array_ptr(c, p, g_array_buffer));
}
H(client_active_texture) { g_client_tex = a.u32(); if (!g_shader) p_glClientActiveTexture(g_client_tex); }
GLuint bound_tex0();
extern GLuint g_bound_2d[8];
ffp::DrawState draw_state() {
    return {g_proj.back().m, g_mv.back().m, {g_texm[0].back().m, g_texm[1].back().m, g_texm[2].back().m}, g_bound_2d,
            g_array_buffer};
}
// ---- the sea (set_water_texture) -----------------------------------------------------------------
std::unordered_set<GLuint> g_water_tex;
bool g_water_draw = false;  // this draw is the sea: marks the stencil (put back after it)
void water_before_draw(Cpu& c, GLuint tex0, GLint first, GLsizei n, GLenum itype, uint32_t iptr) {
    g_water_draw = !g_water_tex.empty() && g_water_tex.count(tex0) && on_screen() && is_world_projection(g_proj.back()) &&
                   g_depth_test && !g_stencil_on && post::active();
    if (!g_water_draw) return;
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, GLint(post::kWaterStencil), post::kWaterStencil);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glStencilMask(post::kWaterStencil);
    // its height: a vertex through the modelview and the inverse camera view (the sea is flat)
    post::Scene& s = g_post_scene;
    if (!s.water_seen && g_cam_view_set && g_dbg_vtx.ptr && !g_dbg_vtx.buf && g_dbg_vtx.size >= 3 && n > 0) {
        uint32_t i = uint32_t(first);
        if (iptr) i = itype == GL_UNSIGNED_SHORT ? M.r16(iptr) : itype == GL_UNSIGNED_BYTE ? M.r8(iptr) : M.r32(iptr);
        const float v[3] = {dbg_component(c, g_dbg_vtx, i, 0), dbg_component(c, g_dbg_vtx, i, 1), dbg_component(c, g_dbg_vtx, i, 2)};
        const Mat4& mv = g_mv.back();
        float e[3], inv[16];
        for (int k = 0; k < 3; ++k) e[k] = mv.m[k] * v[0] + mv.m[4 + k] * v[1] + mv.m[8 + k] * v[2] + mv.m[12 + k];
        if (glmath::invert4(g_cam_view, inv)) {
            s.water_z = inv[2] * e[0] + inv[6] * e[1] + inv[10] * e[2] + inv[14];
            s.water_seen = std::isfinite(s.water_z);
            static int logged = 0;
            if (s.water_seen && logged++ == 0) LOG("GL: the sea (texture %u) is at height %.0f", tex0, s.water_z);
        }
    }
}
void water_after_draw() {
    if (!g_water_draw) return;
    g_water_draw = false;
    glDisable(GL_STENCIL_TEST);  // the game's: off (water_before_draw only marks when it is)
    glStencilFunc(g_st_func, g_st_ref, g_st_vmask);
    glStencilOp(g_st_op[0], g_st_op[1], g_st_op[2]);
    glStencilMask(g_st_wmask);
}

H(draw_arrays) {
    GLenum m = a.u32(); GLint f = a.s32(); GLsizei n = a.s32();
    trace_draw(bound_tex0(), n);
    trace_geometry(c, f, n, 0, 0, bound_tex0());
    post_before_draw(bound_tex0());
    glow_capture(c, m, f, n, 0, 0);
    tint_before_draw();
    fog_before_draw();
    water_before_draw(c, bound_tex0(), f, n, 0, 0);
    if (g_shader) ffp::draw_arrays(m, f, n, draw_state());
    else glDrawArrays(m, f, n);
    water_after_draw();
    ++g_draws;
}
H(draw_elements) {
    GLenum m = a.u32(); GLsizei n = a.s32(); GLenum t = a.u32(); uint32_t ip = a.u32();
    trace_draw(bound_tex0(), n);
    if (!g_element_buffer) trace_geometry(c, 0, n, t, ip, bound_tex0());
    post_before_draw(bound_tex0());
    if (!g_element_buffer) glow_capture(c, m, 0, n, t, ip);
    tint_before_draw();
    fog_before_draw();
    water_before_draw(c, bound_tex0(), 0, n, t, g_element_buffer ? 0 : ip);
    if (g_shader) ffp::draw_elements(m, n, t, array_ptr(c, ip, g_element_buffer), draw_state());
    else glDrawElements(m, n, t, array_ptr(c, ip, g_element_buffer));
    water_after_draw();
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
GLuint bound_tex0() { return g_bound_2d[0]; }
int active_unit() { return g_unit; }

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

// ---- time-of-day tint (game/night_textures) ------------------------------------------------------
// An extra fixed-function texture stage after the game's own: MODULATE(previous, constant colour).
//  - World draws (unit-0 texture registered as a world texture showing its DAY image): g_tint.
//  - Actor draws (people, cars, anything else in the 3D scene whose texture the night module does
//    not manage): g_actor_tint. The game never enables GL lighting; people and cars carry vertex
//    colours the engine lights on the CPU with a night light tuned for the daylight city.
//  - Not tinted: world textures showing their night image (already final), the sky, the HUD
//    (orthographic) and additive glows (lamps, headlights, effects).
float g_tint[3] = {1, 1, 1}, g_actor_tint[3] = {1, 1, 1};
std::unordered_map<GLuint, WorldTex> g_world_tex;
bool is_world_tex(GLuint tex) { return g_world_tex.count(tex) != 0; }
int g_tint_unit = -1;  // fixed-function unit used for the stage (-1: none spare)
bool g_tint_on = false;
float g_stage_color[3] = {-1, -1, -1};  // the colour currently set on the stage

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

bool is_identity(const float* c) { return std::abs(c[0] - 1) + std::abs(c[1] - 1) + std::abs(c[2] - 1) < 0.003f; }
// A 3D draw of the scene (not the sky, not the HUD, not an additive glow).
bool is_actor_draw() {
    const Mat4& p = g_proj.back();
    if (!is_world_projection(p) || is_sky_draw()) return false;
    return !(g_blend && g_blend_dst == GL_ONE);
}

void tint_before_draw() {
    if (g_tint_unit < 0 && !g_shader) return;
    const float* col = nullptr;
    if (auto it = g_world_tex.find(g_bound_2d[0]); it != g_world_tex.end()) {
        if (it->second == WorldTex::Day) col = g_tint;
    } else if (is_actor_draw()) {
        col = g_actor_tint;
    }
    if (col && is_identity(col)) col = nullptr;
    if (g_shader) {  // a shader uniform instead of the extra texture stage
        ffp::set_tint(col);
        return;
    }
    const bool want = col != nullptr;
    const bool recolor = want && std::memcmp(col, g_stage_color, sizeof g_stage_color) != 0;
    if (want == g_tint_on && !recolor) return;
    p_glActiveTexture(GL_TEXTURE0 + g_tint_unit);
    if (want) {
        if (!g_tint_on) glEnable(GL_TEXTURE_2D);
        if (recolor) {
            const GLfloat c[4] = {col[0], col[1], col[2], 1};
            glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, c);
            std::memcpy(g_stage_color, col, sizeof g_stage_color);
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
        ffp::forget_texture(ids[i]);
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
    if (l == 0) g_dbg_tex_size[bound_tex0()] = {w, h};
    if (l == 0 && t == GL_TEXTURE_2D) ffp::set_texture_format(g_bound_2d[g_unit], f);
    const void* px = host(c, a.u32());
#ifdef GANGSTAR_GLES
    // GLES 3 takes BGRA only through an extension with stricter rules: swap to RGBA instead.
    static std::vector<uint8_t> rgba;
    if (f == GL_BGRA && ty == GL_UNSIGNED_BYTE && px) {
        rgba.assign(static_cast<const uint8_t*>(px), static_cast<const uint8_t*>(px) + size_t(w) * h * 4);
        for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
        px = rgba.data();
        f = GL_RGBA;
        ifmt = GL_RGBA;
    }
#endif
    glTexImage2D(t, l, ifmt, w, h, b, f, ty, px);
    // An id refilled with uncompressed data no longer holds a (night-swappable) world texture.
    if (l == 0 && t == GL_TEXTURE_2D && g_tex_observer.on_delete) g_tex_observer.on_delete(g_bound_2d[g_unit]);
}
H(tex_subimage_2d) {
    GLenum t = a.u32(); GLint l = a.s32(), x = a.s32(), y = a.s32(); GLsizei w = a.s32(), h = a.s32();
    GLenum f = a.u32(), ty = a.u32();
    const void* px = host(c, a.u32());
#ifdef GANGSTAR_GLES
    static std::vector<uint8_t> rgba;
    if (f == GL_BGRA && ty == GL_UNSIGNED_BYTE && px) {  // see tex_image_2d
        rgba.assign(static_cast<const uint8_t*>(px), static_cast<const uint8_t*>(px) + size_t(w) * h * 4);
        for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
        px = rgba.data();
        f = GL_RGBA;
    }
#endif
    glTexSubImage2D(t, l, x, y, w, h, f, ty, px);
}
// DXT on GPUs without S3TC (Adreno, Mali): decoded to RGBA8 at upload (GANGSTAR_DECODE_S3TC=1
// forces it on desktop for testing).
bool g_decode_s3tc = false;
void upload_compressed(GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLsizei n, const void* data) {
    if (g_decode_s3tc && s3tc::is_s3tc(ifmt)) {
        static std::vector<uint8_t> rgba;
        if (s3tc::decode(ifmt, w, h, static_cast<const uint8_t*>(data), size_t(n), rgba)) {
            glTexImage2D(t, l, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            return;
        }
        LOG("GL: DXT %#x %dx%d: %d bytes is too short to decode", ifmt, w, h, n);
    }
    p_glCompressedTexImage2D(t, l, ifmt, w, h, 0, n, data);
}

H(compressed_tex_image_2d) {
    GLenum t = a.u32(); GLint l = a.s32(); GLenum ifmt = a.u32(); GLsizei w = a.s32(), h = a.s32(); GLint b = a.s32();
    GLsizei n = a.s32();
    const void* data = host(c, a.u32());
    if (l == 0) g_dbg_tex_size[bound_tex0()] = {w, h};
    if (l == 0 && t == GL_TEXTURE_2D)  // DXT1 without alpha and ETC1 are RGB; the rest has alpha
        ffp::set_texture_format(g_bound_2d[g_unit], ifmt == 0x83F0 || ifmt == 0x8D64 ? GL_RGB : GL_RGBA);
    (void)b;
    upload_compressed(t, l, ifmt, w, h, n, data);
    if (t == GL_TEXTURE_2D && g_tex_observer.on_compressed_upload)
        g_tex_observer.on_compressed_upload(g_bound_2d[g_unit], l, ifmt, w, h, static_cast<const uint8_t*>(data), n);
}
H(compressed_tex_subimage_2d) {
    GLenum t = a.u32(); GLint l = a.s32(), x = a.s32(), y = a.s32(); GLsizei w = a.s32(), h = a.s32();
    GLenum f = a.u32(); GLsizei n = a.s32();
    const void* data = host(c, a.u32());
    if (g_decode_s3tc && s3tc::is_s3tc(f)) {
        static std::vector<uint8_t> rgba;
        if (s3tc::decode(f, w, h, static_cast<const uint8_t*>(data), size_t(n), rgba))
            glTexSubImage2D(t, l, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        return;
    }
    p_glCompressedTexSubImage2D(t, l, x, y, w, h, f, n, data);
}
H(copy_tex_image_2d) {
    GLenum t = a.u32(); GLint l = a.s32(); GLenum ifmt = a.u32(); GLint x = a.s32(), y = a.s32();
    GLsizei w = a.s32(), h = a.s32(); GLint b = a.s32();
    if (l == 0 && t == GL_TEXTURE_2D) ffp::set_texture_format(g_bound_2d[g_unit], ifmt);
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
    trace_event("bind framebuffer %u", g_game_fb);
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
#define LOAD(type, name) GLAPI_LOAD(type, name, ok)
    GL_EXT_FUNCS(LOAD)
#undef LOAD
    if (g_shader && !(ok && ffp::init())) {
#ifdef GANGSTAR_GLES
        LOG("GL: the shader renderer failed to start (see above); nothing can be drawn");
        return false;
#else
        LOG("GL: shader renderer unavailable, using the fixed-function pipeline");
        g_shader = false;
#endif
    }
    LOG("GL: renderer: %s", g_shader ? "shaders (GLES 3 style fixed-function emulation)" : "OpenGL fixed function");
    if (ok && !g_shader) tint_init();
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    g_fog_radial = ext && std::strstr(ext, "GL_NV_fog_distance");
    g_decode_s3tc = !(ext && std::strstr(ext, "GL_EXT_texture_compression_s3tc")) || std::getenv("GANGSTAR_DECODE_S3TC");
    LOG("GL: DXT textures %s", g_decode_s3tc ? "decoded to RGBA (no S3TC on this GPU, or forced)" : "uploaded as they are");
    LOG("GL: fog distance %s", g_fog_radial ? "radial (NV_fog_distance)" : "by depth plane");
    if (ok) post::init();  // settings come from the game (post::set_settings)
    return ok;
}

#ifdef GANGSTAR_GLES
void set_shader_renderer(bool) { g_shader = true; }  // GLES 3 has no fixed-function pipeline
#else
void set_shader_renderer(bool on) { g_shader = on; }
#endif
bool shader_renderer() { return g_shader; }

void set_water_texture(unsigned tex, bool water) {
    if (water) g_water_tex.insert(tex);
    else g_water_tex.erase(tex);
}

void set_camera_view(const float view[16]) {
    if (!glmath::invert4(view, g_cam_inv)) return;
    std::memcpy(g_cam_view, view, sizeof g_cam_view);
    g_cam_view_set = true;
}

void set_fog(const FogConfig& f) {
    if (f.atmospheric == g_fog_cfg.atmospheric && f.far_end == g_fog_cfg.far_end && f.strength == g_fog_cfg.strength &&
        f.fog_everything == g_fog_cfg.fog_everything && f.screen == g_fog_cfg.screen)
        return;
    g_fog_cfg = f;
    apply_fog();
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

void set_world_texture(unsigned tex, WorldTex state) {
    if (state == WorldTex::None) g_world_tex.erase(tex);
    else g_world_tex[tex] = state;
}

void set_actor_tint(float r, float g, float b) {
    g_actor_tint[0] = std::clamp(r, 0.0f, 1.0f);
    g_actor_tint[1] = std::clamp(g, 0.0f, 1.0f);
    g_actor_tint[2] = std::clamp(b, 0.0f, 1.0f);
}

void set_world_tint(float r, float g, float b) {
    const float c[3] = {std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f)};
    std::copy(c, c + 3, g_tint);
}

void replace_compressed_levels(unsigned tex, unsigned format, const std::vector<CompressedLevel>& levels) {
    // Upload through unit 0, then put back what the game had bound there (and its active unit).
    if (g_unit != 0) p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    for (size_t i = 0; i < levels.size(); ++i)
        upload_compressed(GL_TEXTURE_2D, GLint(i), format, levels[i].w, levels[i].h, GLsizei(levels[i].size), levels[i].data);
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
    if (g_post_pending) {  // a scene with no 2D draw after it
        bind_game_fb();
        run_post();
    }
    g_post_view_seen = false;
    g_post_split = false;
    g_post_scene.view_valid = false;
    g_post_scene.water_seen = false;
    if (!post::active() || g_post_scene.glow_count >= post::Scene::kMaxGlows) {  // else the atmosphere stage takes them
        g_post_scene.glow_count = 0;
        g_glow_draws = 0;
    }
    trace_event("present (end of frame %d)", g_frame);
    ++g_frame;
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

void vfb_restore_binding() {
    bind_game_fb();
    if (g_shader) ffp::invalidate();  // the overlay drew with its own program in between
}

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
