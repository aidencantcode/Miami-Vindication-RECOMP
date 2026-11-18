#include "gl/gles1.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
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
bool on_screen() { return g_game_fb == 0; }
int S() { return on_screen() ? g_cfg.scale : 1; }       // scale applied to "screen" coordinates

void make_rb(GLuint* rb, GLenum fmt, int w, int h, int samples) {
    if (!*rb) p_glGenRenderbuffers(1, rb);
    p_glBindRenderbuffer(GL_RENDERBUFFER, *rb);
    if (samples) p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, fmt, w, h);
    else p_glRenderbufferStorage(GL_RENDERBUFFER, fmt, w, h);
}

bool build_vfb() {
    const int w = g_cfg.logical_w * g_cfg.scale, h = g_cfg.logical_h * g_cfg.scale;
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
    LOG("GL: virtual framebuffer %dx%d (logical %dx%d x%d, msaa %d)", w, h, g_cfg.logical_w, g_cfg.logical_h,
        g_cfg.scale, g_cfg.msaa);
    return true;
}

// The single-sample framebuffer holding the current "screen" image (resolving MSAA if needed).
GLuint screen_source() {
    if (!g_cfg.msaa) return g_vfb;
    const int w = g_cfg.logical_w * g_cfg.scale, h = g_cfg.logical_h * g_cfg.scale;
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
H(line_width) { glLineWidth(a.f32() * S()); }
H(point_size) { glPointSize(a.f32() * S()); }
H(point_param_f) {
    GLenum p = a.u32(); float v = a.f32();
    if (p == GL_POINT_SIZE_MIN || p == GL_POINT_SIZE_MAX) v *= S();
    p_glPointParameterf(p, v);
}
H(point_param_fv) { GLenum p = a.u32(); p_glPointParameterfv(p, static_cast<const GLfloat*>(host(c, a.u32()))); }
H(sample_coverage) { float v = a.f32(); GLboolean inv = GLboolean(a.u32()); p_glSampleCoverage(v, inv); }
H(scissor) { GLint x = a.s32(), y = a.s32(); GLsizei w = a.s32(), h = a.s32(); int s = S(); glScissor(x * s, y * s, w * s, h * s); }
H(viewport) { GLint x = a.s32(), y = a.s32(); GLsizei w = a.s32(), h = a.s32(); int s = S(); glViewport(x * s, y * s, w * s, h * s); }
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
H(get_integerv) {
    GLenum p = a.u32();
    GLint* out = static_cast<GLint*>(const_cast<void*>(host(c, a.u32())));
    if (p == GL_FRAMEBUFFER_BINDING) { *out = GLint(g_game_fb); return; }
    glGetIntegerv(p, out);
    if ((p == GL_VIEWPORT || p == GL_SCISSOR_BOX) && S() > 1)
        for (int i = 0; i < 4; ++i) out[i] /= S();
}
H(get_floatv) {
    GLenum p = a.u32();
    GLfloat* out = static_cast<GLfloat*>(const_cast<void*>(host(c, a.u32())));
    glGetFloatv(p, out);
    if ((p == GL_VIEWPORT || p == GL_SCISSOR_BOX) && S() > 1)
        for (int i = 0; i < 4; ++i) out[i] /= float(S());
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
H(fogf) { GLenum p = a.u32(); float v = a.f32(); glFogf(p, v); }
H(fogfv) { GLenum p = a.u32(); glFogfv(p, static_cast<const GLfloat*>(host(c, a.u32()))); }
H(lightf) { GLenum l = a.u32(), p = a.u32(); float v = a.f32(); glLightf(l, p, v); }
H(lightfv) { GLenum l = a.u32(), p = a.u32(); glLightfv(l, p, static_cast<const GLfloat*>(host(c, a.u32()))); }
H(light_modelf) { GLenum p = a.u32(); float v = a.f32(); glLightModelf(p, v); }
H(light_modelfv) { GLenum p = a.u32(); glLightModelfv(p, static_cast<const GLfloat*>(host(c, a.u32()))); }
H(materialf) { GLenum f = a.u32(), p = a.u32(); float v = a.f32(); glMaterialf(f, p, v); }
H(materialfv) { GLenum f = a.u32(), p = a.u32(); glMaterialfv(f, p, static_cast<const GLfloat*>(host(c, a.u32()))); }
H(tex_envf) { GLenum t = a.u32(), p = a.u32(); float v = a.f32(); glTexEnvf(t, p, v); }
H(tex_envi) { GLenum t = a.u32(), p = a.u32(); GLint v = a.s32(); glTexEnvi(t, p, v); }
H(tex_envfv) { GLenum t = a.u32(), p = a.u32(); glTexEnvfv(t, p, static_cast<const GLfloat*>(host(c, a.u32()))); }

// ---- vertex arrays and drawing ---------------------------------------------------------------------
H(vertex_pointer) { GLint s = a.s32(); GLenum t = a.u32(); GLsizei st = a.s32(); glVertexPointer(s, t, st, array_ptr(c, a.u32(), g_array_buffer)); }
H(texcoord_pointer) { GLint s = a.s32(); GLenum t = a.u32(); GLsizei st = a.s32(); glTexCoordPointer(s, t, st, array_ptr(c, a.u32(), g_array_buffer)); }
H(color_pointer) { GLint s = a.s32(); GLenum t = a.u32(); GLsizei st = a.s32(); glColorPointer(s, t, st, array_ptr(c, a.u32(), g_array_buffer)); }
H(normal_pointer) { GLenum t = a.u32(); GLsizei st = a.s32(); glNormalPointer(t, st, array_ptr(c, a.u32(), g_array_buffer)); }
H(client_active_texture) { p_glClientActiveTexture(a.u32()); }
H(draw_arrays) { GLenum m = a.u32(); GLint f = a.s32(); GLsizei n = a.s32(); glDrawArrays(m, f, n); ++g_draws; }
H(draw_elements) {
    GLenum m = a.u32(); GLsizei n = a.s32(); GLenum t = a.u32();
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
H(gen_textures) { GLsizei n = a.s32(); glGenTextures(n, static_cast<GLuint*>(const_cast<void*>(host(c, a.u32())))); }
H(delete_textures) { GLsizei n = a.s32(); glDeleteTextures(n, static_cast<const GLuint*>(host(c, a.u32()))); }
H(bind_texture) { GLenum t = a.u32(); glBindTexture(t, a.u32()); }
H(active_texture) { p_glActiveTexture(a.u32()); }
H(tex_parameteri) { GLenum t = a.u32(), p = a.u32(); GLint v = a.s32(); glTexParameteri(t, p, v); }
H(tex_parameterf) { GLenum t = a.u32(), p = a.u32(); float v = a.f32(); glTexParameterf(t, p, v); }
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
    p_glCompressedTexImage2D(t, l, ifmt, w, h, b, n, host(c, a.u32()));
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
    // Copy from the high-resolution screen: the texture gets scale x the pixels (sharper effects);
    // texture coordinates are normalised, so the game is unaffected.
    const int s = g_cfg.scale;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, screen_source());
    glCopyTexImage2D(t, l, ifmt, x * s, y * s, w * s, h * s, b);
    bind_game_fb();
}
H(generate_mipmap) { p_glGenerateMipmap(a.u32()); }
H(read_pixels) {
    GLint x = a.s32(), y = a.s32(); GLsizei w = a.s32(), h = a.s32(); GLenum f = a.u32(), t = a.u32();
    void* out = const_cast<void*>(host(c, a.u32()));
    if (!on_screen() || (g_cfg.scale == 1 && !g_cfg.msaa)) return glReadPixels(x, y, w, h, f, t, out);
    // Downscale the screen into a logical-size buffer so the game gets exactly w*h pixels.
    const int s = g_cfg.scale;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, screen_source());
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_read_fb);
    p_glBlitFramebuffer(0, 0, g_cfg.logical_w * s, g_cfg.logical_h * s, 0, 0, g_cfg.logical_w, g_cfg.logical_h,
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
    g_game_fb = a.u32();
    p_glBindFramebuffer(t, g_game_fb ? g_game_fb : g_vfb);  // "0" is our virtual screen
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
    if (g_cfg.scale < 1) g_cfg.scale = 1;
    return build_vfb();
}

void vfb_set_scale(int scale) {
    if (scale < 1 || scale == g_cfg.scale) return;
    g_cfg.scale = scale;
    build_vfb();
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

    const int lw = g_cfg.logical_w, lh = g_cfg.logical_h, sw = lw * g_cfg.scale, sh = lh * g_cfg.scale;
    int w = win_w, h = win_h;
    if (fit == Fit::Aspect || fit == Fit::Integer) {
        double k = std::min(double(win_w) / lw, double(win_h) / lh);
        if (fit == Fit::Integer && k >= 1.0) k = std::floor(k);
        w = int(lw * k + 0.5);
        h = int(lh * k + 0.5);
    }
    const int x = (win_w - w) / 2, y = (win_h - h) / 2;  // y from the top
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
