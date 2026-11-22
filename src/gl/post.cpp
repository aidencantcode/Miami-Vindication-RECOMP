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
    X(PFNGLUNIFORM4FVPROC, glUniform4fv)                                        \
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

// Final image: the scene with ground fog / sun haze (fog step 2, 5C.4) plus the glow, which grows
// where the air is thick.
const char* kCompositeFS = R"(
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_scene;
uniform sampler2D u_bloom;
uniform sampler2D u_depth;
uniform float u_bloom_on;
uniform float u_bloom_intensity;
uniform float u_fog_on;
uniform vec4 u_ranges;     // split
uniform vec4 u_nearfar;    // near n, near f, far n, far f
uniform vec2 u_tan;        // tan(fov/2) x, y
uniform mat4 u_inv_view;   // eye -> world
uniform mat4 u_view;       // world -> eye
uniform vec3 u_fog_color;
uniform vec3 u_sun_dir;    // world, toward the sun
uniform float u_sun_warm;  // 0..1: sunrise / sunset
uniform vec4 u_hf;         // density at the reference height, falloff per unit, reference height, amount
#define MAX_LIGHTS 24
uniform int u_light_count;
uniform vec4 u_lights[MAX_LIGHTS];  // world space centre, radius
uniform vec4 u_light_cols[MAX_LIGHTS];  // rgb, w = 1: take the colour from the image at the glow instead
uniform float u_light_int;
uniform sampler2D u_light_src;      // blurred image: each light's colour is read at its glow
float eye_z(float d) {
    float split = u_ranges.x;
    float n, f, ndc;
    if (d <= split) { n = u_nearfar.x; f = u_nearfar.y; ndc = d / split * 2.0 - 1.0; }
    else { n = u_nearfar.z; f = u_nearfar.w; ndc = (d - split) / (1.0 - split) * 2.0 - 1.0; }
    float m10 = (f + n) / (f - n), m14 = -2.0 * f * n / (f - n);
    return m14 / (ndc - m10);
}
void main() {
    vec3 c = texture(u_scene, v_uv).rgb;
    float fog = 0.0;
    float d = texture(u_depth, v_uv).r;
    bool sky = d >= 0.99999;
    float z = sky ? 200000.0 : eye_z(d);   // the sky: a far point along the ray
    vec3 eye = vec3((v_uv.x * 2.0 - 1.0) * u_tan.x * z, (v_uv.y * 2.0 - 1.0) * u_tan.y * z, z);
    // Dynamic lights: pools of coloured light around the game's glows (lamps, neon, car lights), in
    // world space with the height difference counted at 45%, so a lamp up on its pole still lights
    // the street below. Each light's colour is read from the blurred image at its glow.
    if (u_light_count > 0 && !sky) {
        vec3 wp = (u_inv_view * vec4(eye, 1.0)).xyz;
        vec3 acc = vec3(0.0);
        for (int i = 0; i < MAX_LIGHTS; ++i) {
            if (i >= u_light_count) break;
            vec4 L = u_lights[i];
            vec3 dv = wp - L.xyz;
            dv.z *= 0.45;
            float q = dot(dv, dv) / (L.w * L.w);
            if (q < 1.0) {
                // fades in away from the centre: the lamp head / the car itself is not lit by its own glow
                float a = (1.0 - q) * (1.0 - q) * smoothstep(0.02, 0.2, q);
                vec3 lc = u_light_cols[i].rgb;
                if (u_light_cols[i].w > 0.5) {
                    vec3 le = (u_view * vec4(L.xyz, 1.0)).xyz;  // back to eye space for its screen position
                    vec2 luv = clamp(vec2(le.x / (le.z * u_tan.x), le.y / (le.z * u_tan.y)) * 0.5 + 0.5, 0.0, 1.0);
                    lc = texture(u_light_src, luv).rgb;
                    lc /= max(max(lc.r, max(lc.g, lc.b)), 0.08);
                }
                acc += lc * a;
            }
        }
        acc = min(acc, vec3(1.5));
        c += (c * 1.0 + 0.015) * acc * u_light_int;
    }
    if (u_fog_on > 0.5) {
        vec3 cam = (u_inv_view * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
        vec3 world = (u_inv_view * vec4(eye, 1.0)).xyz;
        vec3 ray = world - cam;
        float dist = length(ray);
        vec3 dir = ray / max(dist, 1e-3);
        // Exponential height fog integrated along the ray: density(h) = a * exp(-b (h - h0)).
        float a = u_hf.x, b = u_hf.y;
        float dz = dir.z * b;
        float along = abs(dz) > 1e-5 ? (1.0 - exp(-dz * dist)) / dz : dist;
        float depth_amount = a * exp(-b * (cam.z - u_hf.z)) * along;
        fog = clamp(1.0 - exp(-depth_amount), 0.0, 0.85) * u_hf.w;
        fog = clamp(fog, 0.0, 0.9);
        float sun = pow(max(dot(dir, u_sun_dir), 0.0), 8.0) * u_sun_warm;
        vec3 sun_col = vec3(1.0, 0.62, 0.32);
        vec3 col = mix(u_fog_color, sun_col, clamp(sun, 0.0, 1.0) * 0.8);
        c = mix(c, col, fog);
    }
    vec3 b = texture(u_bloom, v_uv).rgb * u_bloom_on;
    c += b * u_bloom_intensity * (1.0 + 1.5 * fog);
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

// General 4x4 inverse (column-major), false if singular.
bool invert4(const float* m, float* out) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::abs(det) < 1e-12f) return false;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
    return true;
}

// Ground fog parameters (world units are ~1 cm: the player is ~180 tall).
constexpr float kFogFalloff = 1.0f / 1500.0f;   // density halves every ~10 m of height
constexpr float kFogDensity = 1.2e-5f;          // ~30% extra haze over 300 m at the reference height
constexpr float kFogRefBelowCamera = 400.0f;    // reference height: ~4 m below the camera (the street)

// Muzzle flashes (world space), added by the game hooks for the player's shots.
struct Flash { float p[3]; Uint32 t; };
Flash g_flashes[4] = {};
int g_flash_next = 0;

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

void flash(float x, float y, float z) {
    Flash& f = g_flashes[g_flash_next++ % 4];
    f.p[0] = x;
    f.p[1] = y;
    f.p[2] = z;
    f.t = std::max<Uint32>(1, SDL_GetTicks());
}

namespace {
float g_sun_dir[3] = {-0.97f, 0.0f, 0.25f};
}
void set_time_of_day(int minute) {
    // Rises in the east (+x) at 6:00, highest in the south at 12:30, sets in the west (-x) at 19:00.
    const float k = std::clamp((minute / 60.0f - 6.0f) / 13.0f, -0.1f, 1.1f) * 3.14159265f;
    float d[3] = {std::cos(k), -0.35f * std::sin(k), 0.12f + 0.9f * std::max(0.0f, std::sin(k))};
    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; ++i) g_sun_dir[i] = d[i] / l;
}

bool active() { return g_ok && (g_set.test != nullptr || g_set.bloom || g_set.height_fog || g_set.lights); }

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
            // the bloom chain also feeds the light colours, so build it whenever either is on
            const bool want_chain = (g_set.bloom && g_set.bloom_intensity > 0) || (g_set.lights && sc.light_count > 0);
            const bool chain = want_chain && ensure_bloom_targets(sc.w, sc.h);
            if (chain) run_bloom();
            const bool bloom = chain && g_set.bloom && g_set.bloom_intensity > 0;
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
            // ground fog / sun haze
            float inv_view[16];
            const bool have_view = sc.view_valid && invert4(sc.view, inv_view);
            const bool fog = g_set.height_fog && g_set.height_fog_amount > 0 && have_view;
            if (have_view) {
                p_glUniformMatrix4fv(uni(p, "u_inv_view"), 1, GL_FALSE, inv_view);
                p_glUniformMatrix4fv(uni(p, "u_view"), 1, GL_FALSE, sc.view);
            }
            p_glUniform1f(uni(p, "u_fog_on"), fog ? 1.0f : 0.0f);
            p_glUniform4f(uni(p, "u_ranges"), sc.split, 0, 0, 0);
            p_glUniform4f(uni(p, "u_nearfar"), sc.near_n, sc.near_f, sc.far_n, sc.far_f);
            p_glUniform2f(uni(p, "u_tan"), sc.tan_half_x, sc.tan_half_y);
            // Dynamic lights: the nearest glows, skipping near-duplicates (a neon tube is many quads).
            float sel[24][4], cols[24][4];
            int nsel = 0;
            // muzzle flashes first (world space already, fixed colour, ~70 ms)
            const Uint32 now = SDL_GetTicks();
            if (g_set.lights && have_view)
                for (const Flash& f : g_flashes)
                    if (f.t && now - f.t < 70 && nsel < 4) {
                        float e[4] = {0, 0, 0, 750.0f};  // stored in world space: converted to eye space for the
                        for (int i = 0; i < 3; ++i)      // common selection below, then back
                            e[i] = sc.view[i] * f.p[0] + sc.view[4 + i] * f.p[1] + sc.view[8 + i] * f.p[2] + sc.view[12 + i];
                        std::memcpy(sel[nsel], e, sizeof e);
                        const float orange[4] = {1.0f, 0.72f, 0.38f, 0.0f};
                        std::memcpy(cols[nsel], orange, sizeof orange);
                        ++nsel;
                    }
            if (g_set.lights && g_set.lights_intensity > 0 && sc.light_count > 0 && have_view) {
                std::vector<int> idx(size_t(sc.light_count));
                for (int i = 0; i < sc.light_count; ++i) idx[size_t(i)] = i;
                auto d2 = [&](int i) { const float* L = sc.lights[i]; return L[0] * L[0] + L[1] * L[1] + L[2] * L[2]; };
                std::sort(idx.begin(), idx.end(), [&](int a, int b) { return d2(a) < d2(b); });
                for (int i : idx) {
                    const float* L = sc.lights[i];
                    bool dup = false;
                    for (int j = 0; j < nsel && !dup; ++j) {
                        const float dx = sel[j][0] - L[0], dy = sel[j][1] - L[1], dz = sel[j][2] - L[2];
                        dup = dx * dx + dy * dy + dz * dz < 150.0f * 150.0f;
                    }
                    if (dup) continue;
                    std::memcpy(sel[nsel], L, sizeof sel[nsel]);
                    const float from_image[4] = {0, 0, 0, 1};
                    std::memcpy(cols[nsel], from_image, sizeof from_image);
                    if (++nsel == 24) break;
                }
            }
            for (int j = 0; j < nsel; ++j) {  // eye -> world (the shader lights in world space)
                const float e[3] = {sel[j][0], sel[j][1], sel[j][2]};
                for (int i = 0; i < 3; ++i)
                    sel[j][i] = inv_view[i] * e[0] + inv_view[4 + i] * e[1] + inv_view[8 + i] * e[2] + inv_view[12 + i];
            }
            p_glUniform1i(uni(p, "u_light_count"), nsel);
            if (nsel) {
                p_glUniform4fv(uni(p, "u_lights"), nsel, &sel[0][0]);
                p_glUniform4fv(uni(p, "u_light_cols"), nsel, &cols[0][0]);
            }
            p_glUniform1f(uni(p, "u_light_int"), std::clamp(g_set.lights_intensity, 0.0f, 2.0f) * (0.3f + 0.7f * g_night));
            p_glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, bloom && g_level_count > 1 ? g_levels[1].color : g_scene.color);
            p_glActiveTexture(GL_TEXTURE0);
            p_glUniform1i(uni(p, "u_light_src"), 3);
            static int logged_lights = 0;
            if (sc.light_count && logged_lights++ % 1000 == 0)
                LOG("post: %d glow lights this frame, %d used", sc.light_count, nsel);
            if (fog) {
                p_glUniform3f(uni(p, "u_fog_color"), sc.fog_color[0], sc.fog_color[1], sc.fog_color[2]);
                const float* sun = g_sun_dir;  // from the game clock (set_time_of_day)
                p_glUniform3f(uni(p, "u_sun_dir"), sun[0], sun[1], sun[2]);
                // warm haze only when the time-of-day colour is warm (sunrise / sunset)
                const float warm = std::clamp((sc.fog_color[0] - sc.fog_color[2]) * 4.0f, 0.0f, 1.0f);
                p_glUniform1f(uni(p, "u_sun_warm"), warm);
                p_glUniform4f(uni(p, "u_hf"), kFogDensity, kFogFalloff, inv_view[14] - kFogRefBelowCamera,
                              std::clamp(g_set.height_fog_amount, 0.0f, 2.0f));
                static int logged = 0;
                if (logged++ % 1000 == 999)
                    LOG("post: fog camera %.0f %.0f %.0f, sun %.2f %.2f %.2f warm %.2f, fog colour %.2f %.2f %.2f", inv_view[12],
                        inv_view[13], inv_view[14], sun[0], sun[1], sun[2], warm, sc.fog_color[0], sc.fog_color[1],
                        sc.fog_color[2]);
            }
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
