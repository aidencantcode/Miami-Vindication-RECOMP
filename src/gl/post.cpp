#include "gl/post.h"

#include <SDL.h>
#include <SDL_opengl_glext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"

namespace post {

namespace {

// ---- entry points (GL 2.0 / 3.0, loaded at runtime) ------------------------------------------------
#define POST_GL_FUNCS(X)                                                        \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                    \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                    \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                  \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                      \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                            \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                    \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                  \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                    \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                      \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                    \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                          \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                        \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                        \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                          \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                          \
    X(PFNGLUNIFORM2FPROC, glUniform2f)                                          \
    X(PFNGLUNIFORM3FPROC, glUniform3f)                                          \
    X(PFNGLUNIFORM4FPROC, glUniform4f)                                          \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                            \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                                  \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                              \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                              \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                    \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)                              \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)

#define DECLARE(type, name) type p_##name = nullptr;
POST_GL_FUNCS(DECLARE)
#undef DECLARE

bool g_ok = false;
Settings g_set;

// ---- shaders -------------------------------------------------------------------------------------
// One fullscreen triangle from gl_VertexID (no vertex arrays).
const char* kVS = R"(
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Debug passes: invert the scene, or show the reconstructed eye distance (to check the depth input).
const char* kTestFS = R"(
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_scene;
uniform sampler2D u_depth;
uniform int u_mode;
uniform vec4 u_ranges;   // split, unused...
uniform vec4 u_nearfar;  // near n, near f, far n, far f
float eye_z(float d) {
    // Left-handed projection: ndc = m10 + m14 / z with m10 = (f+n)/(f-n), m14 = -2fn/(f-n).
    float split = u_ranges.x;
    float n, f, ndc;
    if (d <= split) { n = u_nearfar.x; f = u_nearfar.y; ndc = d / split * 2.0 - 1.0; }
    else { n = u_nearfar.z; f = u_nearfar.w; ndc = (d - split) / (1.0 - split) * 2.0 - 1.0; }
    float m10 = (f + n) / (f - n), m14 = -2.0 * f * n / (f - n);
    return m14 / (ndc - m10);
}
void main() {
    vec3 c = texture(u_scene, v_uv).rgb;
    if (u_mode == 1) { o_color = vec4(1.0 - c, 1.0); return; }
    float z = eye_z(texture(u_depth, v_uv).r);
    o_color = vec4(vec3(clamp(log2(max(z, 1.0)) / 17.0, 0.0, 1.0)), 1.0);
}
)";

// ---- bloom (5C.5): bright pass, dual-filter blur (Bjorge, ARM 2015), composite ---------------------
// Bright pass at half resolution with a soft threshold on the brightest channel (13 bilinear taps of
// the full-size scene, so thin bright lines don't flicker).
const char* kBrightFS = R"(
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_src;
uniform vec2 u_texel;      // 1 / source size
uniform vec2 u_threshold;  // threshold, knee
vec3 pick(vec2 uv) {
    vec3 c = texture(u_src, uv).rgb;
    float br = max(c.r, max(c.g, c.b));
    float soft = clamp(br - u_threshold.x + u_threshold.y, 0.0, 2.0 * u_threshold.y);
    soft = soft * soft / (4.0 * u_threshold.y + 1e-4);
    float w = max(soft, br - u_threshold.x) / max(br, 1e-4);
    return c * w;
}
void main() {
    vec2 t = u_texel;
    vec3 a = pick(v_uv + t * vec2(-1.0, -1.0)) + pick(v_uv + t * vec2(1.0, -1.0)) +
             pick(v_uv + t * vec2(-1.0, 1.0)) + pick(v_uv + t * vec2(1.0, 1.0));
    vec3 b = pick(v_uv + t * vec2(-2.0, 0.0)) + pick(v_uv + t * vec2(2.0, 0.0)) +
             pick(v_uv + t * vec2(0.0, -2.0)) + pick(v_uv + t * vec2(0.0, 2.0));
    o_color = vec4(pick(v_uv) * 0.25 + a * 0.125 + b * 0.0625, 1.0);
}
)";

const char* kDownFS = R"(
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_src;
uniform vec2 u_texel;  // half a texel of the source
void main() {
    vec3 c = texture(u_src, v_uv).rgb * 4.0;
    c += texture(u_src, v_uv - u_texel).rgb;
    c += texture(u_src, v_uv + u_texel).rgb;
    c += texture(u_src, v_uv + vec2(u_texel.x, -u_texel.y)).rgb;
    c += texture(u_src, v_uv - vec2(u_texel.x, -u_texel.y)).rgb;
    o_color = vec4(c / 8.0, 1.0);
}
)";

const char* kUpFS = R"(
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_src;
uniform vec2 u_texel;  // half a texel of the source, times the radius
void main() {
    vec2 t = u_texel;
    vec3 c = texture(u_src, v_uv + vec2(-t.x * 2.0, 0.0)).rgb;
    c += texture(u_src, v_uv + vec2(-t.x, t.y)).rgb * 2.0;
    c += texture(u_src, v_uv + vec2(0.0, t.y * 2.0)).rgb;
    c += texture(u_src, v_uv + vec2(t.x, t.y)).rgb * 2.0;
    c += texture(u_src, v_uv + vec2(t.x * 2.0, 0.0)).rgb;
    c += texture(u_src, v_uv + vec2(t.x, -t.y)).rgb * 2.0;
    c += texture(u_src, v_uv + vec2(0.0, -t.y * 2.0)).rgb;
    c += texture(u_src, v_uv + vec2(-t.x, -t.y)).rgb * 2.0;
    o_color = vec4(c / 12.0, 1.0);
}
)";

// Final image: the scene (fog step 2 goes here, Q8) plus the glow.
const char* kCompositeFS = R"(
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_scene;
uniform sampler2D u_bloom;
uniform float u_bloom_on;
uniform float u_bloom_intensity;
void main() {
    vec3 c = texture(u_scene, v_uv).rgb;
    vec3 b = texture(u_bloom, v_uv).rgb * u_bloom_on;
    c += b * u_bloom_intensity;
    o_color = vec4(c, 1.0);
}
)";

std::string prelude(GLenum stage) {
#ifdef GANGSTAR_GLES
    return std::string("#version 300 es\n") + (stage == GL_FRAGMENT_SHADER ? "precision highp float;\n" : "");
#else
    (void)stage;
    return "#version 330\n";
#endif
}

GLuint compile(GLenum stage, const char* src, const char* name) {
    const GLuint s = p_glCreateShader(stage);
    const std::string full = prelude(stage) + src;
    const char* p = full.c_str();
    p_glShaderSource(s, 1, &p, nullptr);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        p_glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOG("post: shader %s failed to compile:\n%s", name, log);
        p_glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link(const char* fs_src, const char* name) {
    const GLuint vs = compile(GL_VERTEX_SHADER, kVS, "fullscreen.vs"), fs = compile(GL_FRAGMENT_SHADER, fs_src, name);
    if (!vs || !fs) return 0;
    const GLuint p = p_glCreateProgram();
    p_glAttachShader(p, vs);
    p_glAttachShader(p, fs);
    p_glLinkProgram(p);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint ok = 0;
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        p_glGetProgramInfoLog(p, sizeof log, nullptr, log);
        LOG("post: program %s failed to link:\n%s", name, log);
        return 0;
    }
    return p;
}

GLint uni(GLuint p, const char* n) { return p_glGetUniformLocation(p, n); }

GLuint g_test_prog = 0;

// ---- render targets --------------------------------------------------------------------------------
struct Target {
    GLuint fbo = 0, color = 0, depth = 0;
    int w = 0, h = 0;
};
Target g_scene;  // resolved copy of the scene: colour + depth/stencil textures

GLuint make_tex(int w, int h, GLint ifmt, GLenum fmt, GLenum type, GLint filter) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void free_target(Target& t) {
    if (t.color) glDeleteTextures(1, &t.color);
    if (t.depth) glDeleteTextures(1, &t.depth);
    t.color = t.depth = 0;
    t.w = t.h = 0;
}

bool ensure_scene_target(int w, int h) {
    if (g_scene.w == w && g_scene.h == h && g_scene.fbo) return true;
    free_target(g_scene);
    if (!g_scene.fbo) p_glGenFramebuffers(1, &g_scene.fbo);
    g_scene.color = make_tex(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
    // Same format as the virtual framebuffer's depth (blits need identical depth formats).
    g_scene.depth = make_tex(w, h, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, GL_NEAREST);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_scene.fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_scene.color, 0);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g_scene.depth, 0);
    const GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        LOG("post: scene target %dx%d incomplete (%#x)", w, h, st);
        return false;
    }
    g_scene.w = w;
    g_scene.h = h;
    LOG("post: scene target %dx%d", w, h);
    return true;
}

void draw_fullscreen() { glDrawArrays(GL_TRIANGLES, 0, 3); }

// Bloom chain: level 0 is half the scene size, each next level half the previous.
constexpr int kMaxLevels = 6;
Target g_levels[kMaxLevels];
int g_level_count = 0;
GLuint g_bright_prog = 0, g_down_prog = 0, g_up_prog = 0, g_composite_prog = 0;

bool ensure_bloom_targets(int w, int h) {
    if (g_level_count && g_levels[0].w == std::max(1, w / 2) && g_levels[0].h == std::max(1, h / 2)) return true;
    for (Target& t : g_levels) free_target(t);
    g_level_count = 0;
    int lw = w / 2, lh = h / 2;
    for (int i = 0; i < kMaxLevels && lw >= 8 && lh >= 8; ++i, lw /= 2, lh /= 2) {
        Target& t = g_levels[i];
        if (!t.fbo) p_glGenFramebuffers(1, &t.fbo);
        t.color = make_tex(lw, lh, GL_RGBA16F, GL_RGBA, GL_FLOAT, GL_LINEAR);
        t.w = lw;
        t.h = lh;
        p_glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOG("post: bloom level %d (%dx%d) incomplete", i, lw, lh);
            return false;
        }
        ++g_level_count;
    }
    LOG("post: bloom chain %d levels from %dx%d", g_level_count, w / 2, h / 2);
    return g_level_count > 0;
}

void pass_into(const Target& dst, GLuint prog, GLuint src_tex, float tx, float ty) {
    p_glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    p_glUseProgram(prog);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    p_glUniform1i(uni(prog, "u_src"), 0);
    p_glUniform2f(uni(prog, "u_texel"), tx, ty);
    draw_fullscreen();
}

// How dark the scene is (0 day .. 1 night), from the time-of-day fog colour: lights should glow a
// lot at night while the bright daytime sky (as bright as a neon tube) should only glow a little.
float g_night = 0;

void run_bloom() {
    const Target& s = g_scene;
    // bright pass: full-size scene -> level 0
    const float threshold = std::max(0.2f, g_set.bloom_threshold - 0.15f * g_night);
    p_glUseProgram(g_bright_prog);
    p_glUniform2f(uni(g_bright_prog, "u_threshold"), threshold, std::max(0.05f, threshold * 0.25f));
    pass_into(g_levels[0], g_bright_prog, s.color, 1.0f / s.w, 1.0f / s.h);
    // down
    for (int i = 1; i < g_level_count; ++i)
        pass_into(g_levels[i], g_down_prog, g_levels[i - 1].color, 0.5f / g_levels[i - 1].w, 0.5f / g_levels[i - 1].h);
    // up, adding every scale into the level above
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    const float r = std::clamp(g_set.bloom_radius, 0.25f, 3.0f);
    for (int i = g_level_count - 1; i > 0; --i)
        pass_into(g_levels[i - 1], g_up_prog, g_levels[i].color, r * 0.5f / g_levels[i].w, r * 0.5f / g_levels[i].h);
    glDisable(GL_BLEND);
}

}  // namespace

bool init() {
    bool ok = true;
#define LOAD(type, name)                                                         \
    p_##name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));            \
    if (!p_##name) { LOG("post: missing GL entry point %s", #name); ok = false; }
    POST_GL_FUNCS(LOAD)
#undef LOAD
    if (!ok) return false;
    g_test_prog = link(kTestFS, "test.fs");
    g_bright_prog = link(kBrightFS, "bright.fs");
    g_down_prog = link(kDownFS, "down.fs");
    g_up_prog = link(kUpFS, "up.fs");
    g_composite_prog = link(kCompositeFS, "composite.fs");
    g_ok = g_test_prog && g_bright_prog && g_down_prog && g_up_prog && g_composite_prog;
    LOG("post: %s", g_ok ? "ready" : "disabled (shader errors)");
    return g_ok;
}

void set_settings(const Settings& s) { g_set = s; }

bool active() { return g_ok && (g_set.test != nullptr || g_set.bloom || g_set.height_fog); }

void run(const Scene& sc) {
    if (!active() || sc.w <= 0 || sc.h <= 0) return;
    // Save what the game's fixed-function rendering relies on; the attrib stacks cover enables,
    // viewport, colour/depth masks, texture bindings of every unit and the client arrays.
    GLint prev_draw = 0, prev_read = 0, prev_prog = 0, prev_array = 0, prev_elem = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prev_elem);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);

    for (GLenum cap : {GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_ALPHA_TEST, GL_FOG, GL_LIGHTING,
                       GL_STENCIL_TEST, GL_COLOR_MATERIAL})
        glDisable(cap);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    // No client arrays: the fullscreen triangle comes from gl_VertexID, and an enabled array would
    // still be read (stale guest pointers).
    for (GLenum a : {GL_VERTEX_ARRAY, GL_COLOR_ARRAY, GL_NORMAL_ARRAY, GL_TEXTURE_COORD_ARRAY}) glDisableClientState(a);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    if (ensure_scene_target(sc.w, sc.h)) {
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, sc.fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_scene.fbo);
        p_glBlitFramebuffer(0, 0, sc.w, sc.h, 0, 0, sc.w, sc.h, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);

        p_glActiveTexture(GL_TEXTURE0);
        if (g_set.test) {
            p_glBindFramebuffer(GL_FRAMEBUFFER, sc.fbo);
            glViewport(0, 0, sc.w, sc.h);
            p_glUseProgram(g_test_prog);
            p_glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, g_scene.depth);
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_scene.color);
            p_glUniform1i(uni(g_test_prog, "u_scene"), 0);
            p_glUniform1i(uni(g_test_prog, "u_depth"), 1);
            p_glUniform1i(uni(g_test_prog, "u_mode"), std::strcmp(g_set.test, "invert") == 0 ? 1 : 2);
            p_glUniform4f(uni(g_test_prog, "u_ranges"), sc.split, 0, 0, 0);
            p_glUniform4f(uni(g_test_prog, "u_nearfar"), sc.near_n, sc.near_f, sc.far_n, sc.far_f);
            draw_fullscreen();
        } else {
            const float fog_lum = 0.299f * sc.fog_color[0] + 0.587f * sc.fog_color[1] + 0.114f * sc.fog_color[2];
            g_night = std::clamp((0.5f - fog_lum) / 0.35f, 0.0f, 1.0f);
            const bool bloom = g_set.bloom && g_set.bloom_intensity > 0 && ensure_bloom_targets(sc.w, sc.h);
            if (bloom) run_bloom();
            // composite into the game's framebuffer
            p_glBindFramebuffer(GL_FRAMEBUFFER, sc.fbo);
            glViewport(0, 0, sc.w, sc.h);
            const GLuint p = g_composite_prog;
            p_glUseProgram(p);
            p_glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, bloom ? g_levels[0].color : g_scene.color);
            p_glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, g_scene.depth);
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_scene.color);
            p_glUniform1i(uni(p, "u_scene"), 0);
            p_glUniform1i(uni(p, "u_bloom"), 1);
            p_glUniform1i(uni(p, "u_depth"), 2);
            p_glUniform1f(uni(p, "u_bloom_on"), bloom ? 1.0f : 0.0f);
            // the up chain sums the levels: normalise so intensity means the same at any level count
            p_glUniform1f(uni(p, "u_bloom_intensity"),
                          g_set.bloom_intensity / float(std::max(1, g_level_count)) * 2.0f * (1.0f + 2.0f * g_night));
            draw_fullscreen();
        }
    }

    p_glUseProgram(GLuint(prev_prog));
    glPopClientAttrib();
    glPopAttrib();
    p_glBindBuffer(GL_ARRAY_BUFFER, GLuint(prev_array));
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, GLuint(prev_elem));
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(prev_draw));
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(prev_read));
}

}  // namespace post
