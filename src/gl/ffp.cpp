#include "gl/ffp.h"

#include <SDL.h>
#include "gl/glapi.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

#include "core/log.h"

namespace ffp {

namespace {

// ---- entry points ---------------------------------------------------------------------------------
#define FFP_GL_FUNCS(X)                                                         \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                    \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                    \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                  \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                      \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                            \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                    \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                  \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                    \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation)                        \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                      \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                    \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                          \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                        \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                        \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                          \
    X(PFNGLUNIFORM2IPROC, glUniform2i)                                          \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                          \
    X(PFNGLUNIFORM3FPROC, glUniform3f)                                          \
    X(PFNGLUNIFORM4FPROC, glUniform4f)                                          \
    X(PFNGLUNIFORM4IVPROC, glUniform4iv)                                        \
    X(PFNGLUNIFORM3IVPROC, glUniform3iv)                                        \
    X(PFNGLUNIFORM2FVPROC, glUniform2fv)                                        \
    X(PFNGLUNIFORM4FVPROC, glUniform4fv)                                        \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                            \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                      \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)              \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray)            \
    X(PFNGLVERTEXATTRIB4FPROC, glVertexAttrib4f)                                \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)

#define DECLARE(type, name) type p_##name = nullptr;
FFP_GL_FUNCS(DECLARE)
#undef DECLARE

// ---- GLES 1.1 enums used here (named for readability) ---------------------------------------------
enum : GLenum {
    kCombine = 0x8570, kCombineRgb = 0x8571, kCombineAlpha = 0x8572, kRgbScale = 0x8573, kAddSigned = 0x8574,
    kInterpolate = 0x8575, kConstant = 0x8576, kPrimaryColor = 0x8577, kPrevious = 0x8578, kSubtract = 0x84E7,
    kDot3Rgb = 0x86AE, kDot3Rgba = 0x86AF, kSrc0Rgb = 0x8580, kSrc0Alpha = 0x8588, kOperand0Rgb = 0x8590,
    kOperand0Alpha = 0x8598, kAlphaScale = 0x0D1C, kPointSprite = 0x8861, kCoordReplace = 0x8862,
    kFogDistanceMode = 0x855A, kEyeRadial = 0x855B,
};

// ---- state -----------------------------------------------------------------------------------------
struct Env {
    int mode = 1;                      // 0 REPLACE 1 MODULATE 2 DECAL 3 BLEND 4 ADD 5 COMBINE
    int comb_rgb = 1, comb_a = 1;      // 0 REPLACE 1 MODULATE 2 ADD 3 ADD_SIGNED 4 INTERPOLATE 5 SUBTRACT 6 DOT3_RGB 7 DOT3_RGBA
    int src_rgb[3] = {0, 3, 1}, src_a[3] = {0, 3, 1};  // 0 TEXTURE 1 CONSTANT 2 PRIMARY 3 PREVIOUS
    int op_rgb[3] = {0, 0, 2}, op_a[3] = {2, 2, 2};    // 0 SRC_COLOR 1 1-SRC_COLOR 2 SRC_ALPHA 3 1-SRC_ALPHA
    float scale_rgb = 1, scale_a = 1;
    float color[4] = {0, 0, 0, 0};
    bool coord_replace = false;
};

struct State {
    bool tex2d[kUnits] = {};
    bool alpha_test = false, fog = false, point_sprite = false, lighting = false;
    bool clip[2] = {};
    float clip_plane[2][4] = {};
    bool arr_vertex = false, arr_color = false, arr_normal = false, arr_tc[kUnits] = {};
    Array vertex, colors, tc[kUnits];
    float color[4] = {1, 1, 1, 1};
    float texcoord[kUnits][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
    Env env[kUnits];
    int alpha_func = 8;                // 1 NEVER .. 8 ALWAYS
    float alpha_ref = 0;
    int fog_mode = 2;                  // 1 LINEAR 2 EXP 3 EXP2
    float fog_start = 0, fog_end = 1, fog_density = 1;
    float fog_color[4] = {0, 0, 0, 0};
    bool fog_radial = false;
    int fog_override = 0;              // the bridge: 0 the game's GL_FOG, 1 forced on, -1 forced off
    float point_size = 1;
    float tint[3] = {1, 1, 1};
};
State g;
std::unordered_map<GLuint, int> g_tex_fmt;  // texture -> bit 0: has colour, bit 1: has alpha

int fmt_flags(GLenum base) {
    switch (base) {
        case GL_ALPHA: return 2;
        case GL_LUMINANCE: case GL_RGB: return 1;
        default: return 3;  // GL_LUMINANCE_ALPHA, GL_RGBA, GL_BGRA, unknown
    }
}

int env_mode(GLenum m) {
    switch (m) {
        case GL_REPLACE: return 0;
        case GL_MODULATE: return 1;
        case GL_DECAL: return 2;
        case GL_BLEND: return 3;
        case GL_ADD: return 4;
        case kCombine: return 5;
        default: return 1;
    }
}
int combine_fn(GLenum f) {
    switch (f) {
        case GL_REPLACE: return 0;
        case GL_MODULATE: return 1;
        case GL_ADD: return 2;
        case kAddSigned: return 3;
        case kInterpolate: return 4;
        case kSubtract: return 5;
        case kDot3Rgb: return 6;
        case kDot3Rgba: return 7;
        default: return 1;
    }
}
int source(GLenum s) { return s == GL_TEXTURE ? 0 : s == kConstant ? 1 : s == kPrimaryColor ? 2 : 3; }
int operand(GLenum o) {
    return o == GL_SRC_COLOR ? 0 : o == GL_ONE_MINUS_SRC_COLOR ? 1 : o == GL_SRC_ALPHA ? 2 : 3;
}

// ---- shader ----------------------------------------------------------------------------------------
const char* kVS = R"(
in vec4 a_pos;
in vec4 a_col;
in vec4 a_tc0;
in vec4 a_tc1;
in vec4 a_tc2;
uniform mat4 u_proj;
uniform mat4 u_mv;
uniform mat4 u_tm0;
uniform mat4 u_tm1;
uniform mat4 u_tm2;
uniform float u_psize;
out vec4 v_col;
out vec4 v_tc0;
out vec4 v_tc1;
out vec4 v_tc2;
out vec3 v_eye;
void main() {
    vec4 e = u_mv * a_pos;
    v_eye = e.xyz;
    gl_Position = u_proj * e;
    v_col = clamp(a_col, 0.0, 1.0);
    v_tc0 = u_tm0 * a_tc0;
    v_tc1 = u_tm1 * a_tc1;
    v_tc2 = u_tm2 * a_tc2;
    gl_PointSize = u_psize;
}
)";

const char* kFS = R"(
in vec4 v_col;
in vec4 v_tc0;
in vec4 v_tc1;
in vec4 v_tc2;
in vec3 v_eye;
out vec4 o_color;
uniform sampler2D u_tex0;
uniform sampler2D u_tex1;
uniform sampler2D u_tex2;
uniform int u_units;         // bit per unit: texturing on
uniform int u_sprite;        // bit per unit: point sprite coordinates replace the texcoords
uniform ivec4 u_env[3];      // mode, combine rgb, combine alpha, format bits (1 colour, 2 alpha)
uniform ivec3 u_srgb[3];
uniform ivec3 u_salpha[3];
uniform ivec3 u_orgb[3];
uniform ivec3 u_oalpha[3];
uniform vec2 u_scale[3];     // rgb scale, alpha scale
uniform vec4 u_envcol[3];
uniform vec3 u_tint;
uniform ivec2 u_alpha;       // test function (0 off, 1 NEVER .. 8 ALWAYS), unused
uniform float u_alpha_ref;
uniform ivec2 u_fog;         // mode (0 off, 1 LINEAR, 2 EXP, 3 EXP2), radial
uniform vec3 u_fog_p;        // start, end, density
uniform vec3 u_fog_col;
uniform ivec2 u_clip;
uniform vec4 u_clip0;
uniform vec4 u_clip1;

vec4 src(int s, vec4 t, vec4 prev, vec4 cc) {
    if (s == 0) return t;
    if (s == 1) return cc;
    if (s == 2) return v_col;
    return prev;
}
vec3 oprgb(int o, vec4 v) {
    if (o == 0) return v.rgb;
    if (o == 1) return vec3(1.0) - v.rgb;
    if (o == 2) return vec3(v.a);
    return vec3(1.0 - v.a);
}
float opa(int o, vec4 v) { return (o == 2 || o == 0) ? v.a : 1.0 - v.a; }

vec4 stage(int u, vec4 prev, vec4 t) {
    ivec4 e = u_env[u];
    bool hasc = (e.w & 1) != 0, hasa = (e.w & 2) != 0;
    vec4 cc = u_envcol[u];
    vec3 Cf = prev.rgb;
    float Af = prev.a;
    vec4 r;
    if (e.x == 0) {         // REPLACE
        r = vec4(hasc ? t.rgb : Cf, hasa ? t.a : Af);
    } else if (e.x == 1) {  // MODULATE
        r = vec4(hasc ? Cf * t.rgb : Cf, hasa ? Af * t.a : Af);
    } else if (e.x == 2) {  // DECAL
        r = vec4(hasa ? mix(Cf, t.rgb, t.a) : t.rgb, Af);
    } else if (e.x == 3) {  // BLEND
        r = vec4(hasc ? mix(Cf, cc.rgb, t.rgb) : Cf, hasa ? Af * t.a : Af);
    } else if (e.x == 4) {  // ADD
        r = vec4(hasc ? Cf + t.rgb : Cf, hasa ? Af * t.a : Af);
    } else {                // COMBINE
        vec3 a0 = oprgb(u_orgb[u].x, src(u_srgb[u].x, t, prev, cc));
        vec3 a1 = oprgb(u_orgb[u].y, src(u_srgb[u].y, t, prev, cc));
        vec3 a2 = oprgb(u_orgb[u].z, src(u_srgb[u].z, t, prev, cc));
        float b0 = opa(u_oalpha[u].x, src(u_salpha[u].x, t, prev, cc));
        float b1 = opa(u_oalpha[u].y, src(u_salpha[u].y, t, prev, cc));
        float b2 = opa(u_oalpha[u].z, src(u_salpha[u].z, t, prev, cc));
        vec3 c;
        int f = e.y;
        if (f == 0) c = a0;
        else if (f == 1) c = a0 * a1;
        else if (f == 2) c = a0 + a1;
        else if (f == 3) c = a0 + a1 - 0.5;
        else if (f == 4) c = a0 * a2 + a1 * (vec3(1.0) - a2);
        else if (f == 5) c = a0 - a1;
        else c = vec3(4.0 * dot(a0 - 0.5, a1 - 0.5));
        float al;
        int g = e.z;
        if (g == 0) al = b0;
        else if (g == 1) al = b0 * b1;
        else if (g == 2) al = b0 + b1;
        else if (g == 3) al = b0 + b1 - 0.5;
        else if (g == 4) al = b0 * b2 + b1 * (1.0 - b2);
        else al = b0 - b1;
        if (f == 7) al = c.r;
        r = vec4(c * u_scale[u].x, al * u_scale[u].y);
    }
    return clamp(r, 0.0, 1.0);
}

vec2 coords(int u, vec4 tc) {
    if ((u_sprite & (1 << u)) != 0) return vec2(gl_PointCoord.x, 1.0 - gl_PointCoord.y);
    return tc.xy / tc.w;
}

bool alpha_pass(float a) {
    int f = u_alpha.x;
    if (f == 1) return false;
    if (f == 2) return a < u_alpha_ref;
    if (f == 3) return a == u_alpha_ref;
    if (f == 4) return a <= u_alpha_ref;
    if (f == 5) return a > u_alpha_ref;
    if (f == 6) return a != u_alpha_ref;
    if (f == 7) return a >= u_alpha_ref;
    return true;
}

void main() {
    if ((u_clip.x != 0 && dot(vec4(v_eye, 1.0), u_clip0) < 0.0) ||
        (u_clip.y != 0 && dot(vec4(v_eye, 1.0), u_clip1) < 0.0)) discard;
    vec4 c = v_col;
    if ((u_units & 1) != 0) c = stage(0, c, texture(u_tex0, coords(0, v_tc0)));
    if ((u_units & 2) != 0) c = stage(1, c, texture(u_tex1, coords(1, v_tc1)));
    if ((u_units & 4) != 0) c = stage(2, c, texture(u_tex2, coords(2, v_tc2)));
    c.rgb *= u_tint;
    if (u_alpha.x != 0 && !alpha_pass(c.a)) discard;
    if (u_fog.x != 0) {
        float z = u_fog.y != 0 ? length(v_eye) : abs(v_eye.z);
        float f;
        if (u_fog.x == 1) f = (u_fog_p.y - z) / max(u_fog_p.y - u_fog_p.x, 1e-4);
        else if (u_fog.x == 2) f = exp(-u_fog_p.z * z);
        else { float dz = u_fog_p.z * z; f = exp(-dz * dz); }
        c.rgb = mix(u_fog_col, c.rgb, clamp(f, 0.0, 1.0));
    }
    o_color = c;
}
)";

GLuint g_prog = 0;
bool g_ok = false, g_bound_prog = false;
struct Locs {
    GLint proj, mv, tm[kUnits], psize, tex[kUnits], units, sprite, env, srgb, salpha, orgb, oalpha, scale, envcol, tint, alpha,
        alpha_ref, fog, fog_p, fog_col, clip, clip0, clip1;
} L;
enum { A_POS = 0, A_COL = 1, A_TC0 = 2 };

std::string prelude() {
#ifdef GANGSTAR_GLES
    return "#version 300 es\nprecision highp float;\nprecision highp int;\n";
#else
    return "#version 330\n";
#endif
}

GLuint compile(GLenum stage, const char* src) {
    const GLuint s = p_glCreateShader(stage);
    const std::string full = prelude() + src;
    const char* p = full.c_str();
    p_glShaderSource(s, 1, &p, nullptr);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        p_glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOG("ffp: %s shader failed:\n%s", stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        p_glDeleteShader(s);
        return 0;
    }
    return s;
}

// Last uploaded uniform values, so a draw only sends what changed.
struct Cache {
    int units = -1, sprite = -1;
    int env[kUnits][16] = {};
    bool env_valid = false;
    float tint[3] = {-1, -1, -1};
    int alpha[2] = {-1, -1};
    float alpha_ref = -1;
    int fog[2] = {-1, -1};
    float fog_p[3] = {-1, -1, -1}, fog_col[3] = {-1, -1, -1};
    int clip[2] = {-1, -1};
    float psize = -1;
    float tm[kUnits][16] = {};
    bool tm_valid = false;
} C;

void upload(const DrawState& s, GLenum mode) {
    if (!g_bound_prog) {
        p_glUseProgram(g_prog);
        g_bound_prog = true;
        C = Cache{};
    }
    p_glUniformMatrix4fv(L.proj, 1, GL_FALSE, s.proj);
    p_glUniformMatrix4fv(L.mv, 1, GL_FALSE, s.modelview);
    for (int u = 0; u < kUnits; ++u)
        if (!C.tm_valid || std::memcmp(C.tm[u], s.texmat[u], sizeof C.tm[u]) != 0) {
            p_glUniformMatrix4fv(L.tm[u], 1, GL_FALSE, s.texmat[u]);
            std::memcpy(C.tm[u], s.texmat[u], sizeof C.tm[u]);
        }
    C.tm_valid = true;

    int units = 0, sprite = 0;
    int env[kUnits][16];
    for (int u = 0; u < kUnits; ++u) {
        const GLuint tex = s.bound[u];
        if (g.tex2d[u] && tex) units |= 1 << u;
        if (g.point_sprite && mode == GL_POINTS && g.env[u].coord_replace) sprite |= 1 << u;
        const Env& e = g.env[u];
        auto it = g_tex_fmt.find(tex);
        const int fmt = it != g_tex_fmt.end() ? it->second : 3;
        const int v[16] = {e.mode,      e.comb_rgb,  e.comb_a,    fmt,         e.src_rgb[0], e.src_rgb[1],
                           e.src_rgb[2], e.src_a[0], e.src_a[1],  e.src_a[2],  e.op_rgb[0],  e.op_rgb[1],
                           e.op_rgb[2],  e.op_a[0],  e.op_a[1],   e.op_a[2]};
        std::memcpy(env[u], v, sizeof v);
    }
    if (units != C.units) p_glUniform1i(L.units, C.units = units);
    if (sprite != C.sprite) p_glUniform1i(L.sprite, C.sprite = sprite);
    if (!C.env_valid || std::memcmp(env, C.env, sizeof env) != 0) {
        int e4[kUnits * 4], srgb[kUnits * 3], sa[kUnits * 3], orgb[kUnits * 3], oa[kUnits * 3];
        for (int u = 0; u < kUnits; ++u) {
            for (int k = 0; k < 4; ++k) e4[u * 4 + k] = env[u][k];
            for (int k = 0; k < 3; ++k) {
                srgb[u * 3 + k] = env[u][4 + k];
                sa[u * 3 + k] = env[u][7 + k];
                orgb[u * 3 + k] = env[u][10 + k];
                oa[u * 3 + k] = env[u][13 + k];
            }
        }
        p_glUniform4iv(L.env, kUnits, e4);
        p_glUniform3iv(L.srgb, kUnits, srgb);
        p_glUniform3iv(L.salpha, kUnits, sa);
        p_glUniform3iv(L.orgb, kUnits, orgb);
        p_glUniform3iv(L.oalpha, kUnits, oa);
        std::memcpy(C.env, env, sizeof env);
        C.env_valid = true;
    }
    {  // scales and environment colours change rarely; cheap to send
        float sc[kUnits * 2], col[kUnits * 4];
        for (int u = 0; u < kUnits; ++u) {
            sc[u * 2] = g.env[u].scale_rgb;
            sc[u * 2 + 1] = g.env[u].scale_a;
            std::memcpy(col + u * 4, g.env[u].color, sizeof g.env[u].color);
        }
        p_glUniform2fv(L.scale, kUnits, sc);
        p_glUniform4fv(L.envcol, kUnits, col);
    }
    if (std::memcmp(g.tint, C.tint, sizeof C.tint) != 0) {
        p_glUniform3f(L.tint, g.tint[0], g.tint[1], g.tint[2]);
        std::memcpy(C.tint, g.tint, sizeof C.tint);
    }
    if (g.alpha_ref != C.alpha_ref) p_glUniform1f(L.alpha_ref, C.alpha_ref = g.alpha_ref);
    const float fog_p[3] = {g.fog_start, g.fog_end, g.fog_density};
    if (std::memcmp(fog_p, C.fog_p, sizeof fog_p) != 0) {
        p_glUniform3f(L.fog_p, fog_p[0], fog_p[1], fog_p[2]);
        std::memcpy(C.fog_p, fog_p, sizeof fog_p);
    }
    if (std::memcmp(g.fog_color, C.fog_col, sizeof C.fog_col) != 0) {
        p_glUniform3f(L.fog_col, g.fog_color[0], g.fog_color[1], g.fog_color[2]);
        std::memcpy(C.fog_col, g.fog_color, sizeof C.fog_col);
    }
    if (g.point_size != C.psize) p_glUniform1f(L.psize, C.psize = g.point_size);
}


// The ivec2 switches (fog mode / radial, alpha test function, clip planes), cached like the rest.
void upload_ivecs() {
    const bool fog_on = g.fog_override ? g.fog_override > 0 : g.fog;
    const int fog[2] = {fog_on ? g.fog_mode : 0, g.fog_radial ? 1 : 0};
    if (fog[0] != C.fog[0] || fog[1] != C.fog[1]) {
        p_glUniform2i(L.fog, fog[0], fog[1]);
        C.fog[0] = fog[0];
        C.fog[1] = fog[1];
    }
    const int alpha = g.alpha_test ? g.alpha_func : 0;
    if (alpha != C.alpha[0]) p_glUniform2i(L.alpha, C.alpha[0] = alpha, 0);
    const int clip[2] = {g.clip[0] ? 1 : 0, g.clip[1] ? 1 : 0};
    if (clip[0] != C.clip[0] || clip[1] != C.clip[1]) {
        p_glUniform2i(L.clip, clip[0], clip[1]);
        C.clip[0] = clip[0];
        C.clip[1] = clip[1];
    }
    if (g.clip[0]) p_glUniform4f(L.clip0, g.clip_plane[0][0], g.clip_plane[0][1], g.clip_plane[0][2], g.clip_plane[0][3]);
    if (g.clip[1]) p_glUniform4f(L.clip1, g.clip_plane[1][0], g.clip_plane[1][1], g.clip_plane[1][2], g.clip_plane[1][3]);
}

void attrib(GLuint loc, bool on, const Array& a, bool is_color, const float* current) {
    if (on && (a.ptr || a.buffer)) {
        p_glBindBuffer(GL_ARRAY_BUFFER, a.buffer);
        const bool normalized = is_color && a.type != GL_FLOAT && a.type != GL_FIXED;
        p_glVertexAttribPointer(loc, a.size, a.type, normalized ? GL_TRUE : GL_FALSE, a.stride, a.ptr);
        p_glEnableVertexAttribArray(loc);
    } else {
        p_glDisableVertexAttribArray(loc);
        p_glVertexAttrib4f(loc, current[0], current[1], current[2], current[3]);
    }
}

void setup(GLenum mode, const DrawState& s) {
    upload(s, mode);
    upload_ivecs();
    static const float origin[4] = {0, 0, 0, 1};
    attrib(A_POS, g.arr_vertex, g.vertex, false, origin);
    attrib(A_COL, g.arr_color, g.colors, true, g.color);
    for (int u = 0; u < kUnits; ++u) attrib(A_TC0 + u, g.arr_tc[u], g.tc[u], false, g.texcoord[u]);
    p_glBindBuffer(GL_ARRAY_BUFFER, s.game_array_buffer);
    if (g.lighting) {
        static bool warned = false;
        if (!warned) LOG("STUB: ffp: GL_LIGHTING enabled (not emulated)");
        warned = true;
    }
}

}  // namespace

bool init() {
    bool ok = true;
#define LOAD(type, name) GLAPI_LOAD(type, name, ok)
    FFP_GL_FUNCS(LOAD)
#undef LOAD
    if (!ok) return false;
    const GLuint vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) return false;
    g_prog = p_glCreateProgram();
    p_glAttachShader(g_prog, vs);
    p_glAttachShader(g_prog, fs);
    p_glBindAttribLocation(g_prog, A_POS, "a_pos");
    p_glBindAttribLocation(g_prog, A_COL, "a_col");
    p_glBindAttribLocation(g_prog, A_TC0, "a_tc0");
    p_glBindAttribLocation(g_prog, A_TC0 + 1, "a_tc1");
    p_glBindAttribLocation(g_prog, A_TC0 + 2, "a_tc2");
    p_glLinkProgram(g_prog);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint linked = 0;
    p_glGetProgramiv(g_prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096] = {};
        p_glGetProgramInfoLog(g_prog, sizeof log, nullptr, log);
        LOG("ffp: link failed:\n%s", log);
        return false;
    }
    auto u = [](const char* n) { return p_glGetUniformLocation(g_prog, n); };
    L.proj = u("u_proj");
    L.mv = u("u_mv");
    L.tm[0] = u("u_tm0");
    L.tm[1] = u("u_tm1");
    L.tm[2] = u("u_tm2");
    L.psize = u("u_psize");
    L.tex[0] = u("u_tex0");
    L.tex[1] = u("u_tex1");
    L.tex[2] = u("u_tex2");
    L.units = u("u_units");
    L.sprite = u("u_sprite");
    L.env = u("u_env");
    L.srgb = u("u_srgb");
    L.salpha = u("u_salpha");
    L.orgb = u("u_orgb");
    L.oalpha = u("u_oalpha");
    L.scale = u("u_scale");
    L.envcol = u("u_envcol");
    L.tint = u("u_tint");
    L.alpha = u("u_alpha");
    L.alpha_ref = u("u_alpha_ref");
    L.fog = u("u_fog");
    L.fog_p = u("u_fog_p");
    L.fog_col = u("u_fog_col");
    L.clip = u("u_clip");
    L.clip0 = u("u_clip0");
    L.clip1 = u("u_clip1");
    p_glUseProgram(g_prog);
    for (int i = 0; i < kUnits; ++i) p_glUniform1i(L.tex[i], i);
    p_glUseProgram(0);
#ifndef GANGSTAR_GLES
    glEnable(0x8642);  // GL_PROGRAM_POINT_SIZE: gl_PointSize from the shader (always on in GLES 3)
#endif
    g_ok = true;
    LOG("ffp: fixed-function shader ready");
    return true;
}

bool ready() { return g_ok; }
void invalidate() { g_bound_prog = false; }

bool is_ffp_cap(GLenum cap) {
    switch (cap) {
        case GL_TEXTURE_2D: case GL_ALPHA_TEST: case GL_FOG: case GL_LIGHTING: case GL_COLOR_MATERIAL:
        case GL_NORMALIZE: case GL_RESCALE_NORMAL: case GL_POINT_SMOOTH: case GL_LINE_SMOOTH: case kPointSprite:
        case GL_CLIP_PLANE0: case GL_CLIP_PLANE0 + 1: case GL_CLIP_PLANE0 + 2: case GL_CLIP_PLANE0 + 3:
        case GL_CLIP_PLANE0 + 4: case GL_CLIP_PLANE0 + 5: case GL_COLOR_LOGIC_OP: case GL_MULTISAMPLE:
        case GL_SAMPLE_ALPHA_TO_ONE:
            return true;
        default:
            return cap >= GL_LIGHT0 && cap <= GL_LIGHT0 + 7;
    }
}

void enable(GLenum cap, bool on, int unit) {
    switch (cap) {
        case GL_TEXTURE_2D: if (unit >= 0 && unit < kUnits) g.tex2d[unit] = on; break;
        case GL_ALPHA_TEST: g.alpha_test = on; break;
        case GL_FOG: g.fog = on; break;
        case GL_LIGHTING: g.lighting = on; break;
        case kPointSprite: g.point_sprite = on; break;
        case GL_CLIP_PLANE0: g.clip[0] = on; break;
        case GL_CLIP_PLANE0 + 1: g.clip[1] = on; break;
        default: break;  // no effect in this renderer
    }
}

bool enabled(GLenum cap, int unit) {
    switch (cap) {
        case GL_TEXTURE_2D: return unit >= 0 && unit < kUnits && g.tex2d[unit];
        case GL_ALPHA_TEST: return g.alpha_test;
        case GL_FOG: return g.fog;
        case GL_LIGHTING: return g.lighting;
        default: return false;
    }
}

void client_state(GLenum array, bool on, int unit) {
    switch (array) {
        case GL_VERTEX_ARRAY: g.arr_vertex = on; break;
        case GL_COLOR_ARRAY: g.arr_color = on; break;
        case GL_TEXTURE_COORD_ARRAY: if (unit >= 0 && unit < kUnits) g.arr_tc[unit] = on; break;
        case GL_NORMAL_ARRAY: g.arr_normal = on; break;  // not drawn (no lighting); queried by the bridge
        default: break;
    }
}

bool array_enabled(GLenum array) {
    return array == GL_VERTEX_ARRAY ? g.arr_vertex : array == GL_COLOR_ARRAY ? g.arr_color
         : array == GL_NORMAL_ARRAY ? g.arr_normal : false;
}

void color(float r, float gg, float b, float a) {
    g.color[0] = r;
    g.color[1] = gg;
    g.color[2] = b;
    g.color[3] = a;
}

void multi_texcoord(int unit, float s, float t, float r, float q) {
    if (unit < 0 || unit >= kUnits) return;
    const float v[4] = {s, t, r, q};
    std::memcpy(g.texcoord[unit], v, sizeof v);
}

void vertex_pointer(const Array& a) { g.vertex = a; }
void color_pointer(const Array& a) { g.colors = a; }
void texcoord_pointer(int unit, const Array& a) {
    if (unit >= 0 && unit < kUnits) g.tc[unit] = a;
}

void tex_env(int unit, GLenum target, GLenum pname, const float* v, int n) {
    if (unit < 0 || unit >= kUnits || n < 1) return;
    Env& e = g.env[unit];
    if (target == kPointSprite) {
        if (pname == kCoordReplace) e.coord_replace = v[0] != 0;
        return;
    }
    const GLenum iv = GLenum(v[0]);
    if (pname == GL_TEXTURE_ENV_MODE) e.mode = env_mode(iv);
    else if (pname == GL_TEXTURE_ENV_COLOR && n >= 4) std::memcpy(e.color, v, sizeof e.color);
    else if (pname == kCombineRgb) e.comb_rgb = combine_fn(iv);
    else if (pname == kCombineAlpha) e.comb_a = combine_fn(iv);
    else if (pname >= kSrc0Rgb && pname <= kSrc0Rgb + 2) e.src_rgb[pname - kSrc0Rgb] = source(iv);
    else if (pname >= kSrc0Alpha && pname <= kSrc0Alpha + 2) e.src_a[pname - kSrc0Alpha] = source(iv);
    else if (pname >= kOperand0Rgb && pname <= kOperand0Rgb + 2) e.op_rgb[pname - kOperand0Rgb] = operand(iv);
    else if (pname >= kOperand0Alpha && pname <= kOperand0Alpha + 2) e.op_a[pname - kOperand0Alpha] = operand(iv);
    else if (pname == kRgbScale) e.scale_rgb = v[0];
    else if (pname == kAlphaScale) e.scale_a = v[0];
}

void fog(GLenum pname, const float* v, int n) {
    if (n < 1) return;
    switch (pname) {
        case GL_FOG_MODE: {
            const GLenum m = GLenum(v[0]);
            g.fog_mode = m == GL_LINEAR ? 1 : m == GL_EXP ? 2 : 3;
            break;
        }
        case GL_FOG_START: g.fog_start = v[0]; break;
        case GL_FOG_END: g.fog_end = v[0]; break;
        case GL_FOG_DENSITY: g.fog_density = v[0]; break;
        case GL_FOG_COLOR: if (n >= 4) std::memcpy(g.fog_color, v, sizeof g.fog_color); break;
        case kFogDistanceMode: g.fog_radial = GLenum(v[0]) == kEyeRadial; break;
        default: break;
    }
}

void alpha_func(GLenum func, float ref) {
    g.alpha_func = int(func) - int(GL_NEVER) + 1;
    g.alpha_ref = std::clamp(ref, 0.0f, 1.0f);
}

void point_size(float s) { g.point_size = std::max(1.0f, s); }

void clip_plane(int i, const float eye_plane[4]) {
    if (i >= 0 && i < 2) std::memcpy(g.clip_plane[i], eye_plane, sizeof g.clip_plane[i]);
}

void set_fog_override(int o) { g.fog_override = o; }

void set_tint(const float* rgb) {
    if (rgb) std::memcpy(g.tint, rgb, sizeof g.tint);
    else g.tint[0] = g.tint[1] = g.tint[2] = 1;
}

void set_texture_format(GLuint tex, GLenum base_format) { g_tex_fmt[tex] = fmt_flags(base_format); }
void forget_texture(GLuint tex) { g_tex_fmt.erase(tex); }

void draw_arrays(GLenum mode, GLint first, GLsizei count, const DrawState& s) {
    if (!g_ok) return;
    setup(mode, s);
    glDrawArrays(mode, first, count);
}

void draw_elements(GLenum mode, GLsizei count, GLenum type, const void* indices, const DrawState& s) {
    if (!g_ok) return;
    setup(mode, s);
    glDrawElements(mode, count, type, indices);
}

}  // namespace ffp
