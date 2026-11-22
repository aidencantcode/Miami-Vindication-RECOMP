#include "gl/post.h"

#include <SDL.h>
#include "gl/glapi.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "gl/glmath.h"

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
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                        \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray)            \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                          \
    X(PFNGLDRAWARRAYSINSTANCEDPROC, glDrawArraysInstanced)

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
    float d = texture(u_depth, v_uv).r;
    float z = eye_z(d);
    if (u_mode == 3) {  // what the composite treats as sky: magenta (cleared depth) / cyan (past the far city)
        if (d >= 0.99999) o_color = vec4(1.0, 0.0, 1.0, 1.0);
        else if (d > u_ranges.x && z > u_nearfar.w * 0.97) o_color = vec4(0.0, 1.0, 1.0, 1.0);
        else o_color = vec4(c * 0.5, 1.0);
        return;
    }
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

// Final image, in two stages (post.h): the atmosphere (dynamic lights, fog, the sky's horizon) and
// the finish (bloom, colour grade). Fog colour is a gradient by the view ray: the horizon colour low,
// an upper colour higher up, a warm glow toward the sun's side (the Miami palette, by time of day);
// the sky near the horizon takes the same colour, so the fogged city and the sky meet.
const char* kCompositeFS = R"(
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_scene;
uniform sampler2D u_bloom;
uniform sampler2D u_depth;
uniform sampler2D u_light_acc;
uniform vec4 u_stage;      // atmosphere on, finish on, these are sea pixels (stencil pass), light debug
uniform vec4 u_ranges;     // split
uniform vec4 u_nearfar;    // near n, near f, far n, far f
uniform vec2 u_tan;        // tan(fov/2) x, y
uniform mat4 u_inv_view;   // eye -> world
uniform vec3 u_col_horizon;
uniform vec3 u_col_zenith;
uniform vec3 u_col_sun;
uniform vec3 u_col_ground;
uniform vec3 u_sun_dir;    // world, toward the sun
uniform vec4 u_grad;       // ray z where the upper colour has taken over, sun glow width (power), sun glow amount, -
uniform vec4 u_haze;       // haze by distance: exponent scale, 1 / far end, power, the sea's share
uniform vec4 u_hf;         // ground fog: density at the reference height, falloff per unit, reference height, amount
uniform vec2 u_sky_haze;   // on (0..1), ray z where the horizon colour fades out of the sky
uniform float u_light_on;
uniform float u_light_int;
uniform float u_bloom_on;
uniform float u_bloom_intensity;
uniform vec4 u_grade;      // saturation, -, -, -
uniform vec2 u_water;      // the sea's height (world), known (1) or not (0)
uniform vec3 u_tint_lo;    // multiplier for the shadows
uniform vec3 u_tint_hi;    // and for the highlights
float eye_z(float d) {
    float split = u_ranges.x;
    float n, f, ndc;
    if (d <= split) { n = u_nearfar.x; f = u_nearfar.y; ndc = d / split * 2.0 - 1.0; }
    else { n = u_nearfar.z; f = u_nearfar.w; ndc = (d - split) / (1.0 - split) * 2.0 - 1.0; }
    float m10 = (f + n) / (f - n), m14 = -2.0 * f * n / (f - n);
    return m14 / (ndc - m10);
}
vec3 fog_colour(vec3 dir) {
    float up = smoothstep(-0.03, u_grad.x, dir.z);
    vec3 c = mix(u_col_horizon, u_col_zenith, up);
    vec2 h = dir.xy / max(length(dir.xy), 1e-4);
    vec2 s = u_sun_dir.xy / max(length(u_sun_dir.xy), 1e-4);
    float toward = pow(dot(h, s) * 0.5 + 0.5, u_grad.y);
    return mix(c, u_col_sun, clamp(toward * (1.0 - 0.6 * up) * u_grad.z, 0.0, 1.0));
}
// haze by distance and ground fog (exponential in height, integrated along the ray)
vec2 fog_amounts(float dist, vec3 dir, vec3 cam) {
    float haze = u_haze.x > 0.0 ? 1.0 - exp(-u_haze.x * pow(dist * u_haze.y, u_haze.z)) : 0.0;
    float ground = 0.0;
    if (u_hf.w > 0.0) {
        float b = u_hf.y, dz = dir.z * b;
        float along = abs(dz) > 1e-5 ? (1.0 - exp(-dz * dist)) / dz : dist;
        ground = clamp((1.0 - exp(-u_hf.x * exp(-b * (cam.z - u_hf.z)) * along)) * u_hf.w, 0.0, 0.9);
    }
    return vec2(haze, ground);
}
vec3 atmosphere(vec3 c) {
    float d = texture(u_depth, v_uv).r;
    bool sky = d >= 0.99999;
    bool sea = u_stage.z > 0.5;
    float z = sky ? 200000.0 : eye_z(d);
    vec3 eye = vec3((v_uv * 2.0 - 1.0) * u_tan * z, z);
    if (u_light_on > 0.5 && !sky && !sea) {
        // saturating: many overlapping lights brighten a wall up to ~2x, never blow it out
        vec3 acc = 1.0 - exp(-texture(u_light_acc, v_uv).rgb * u_light_int);
        if (u_stage.w > 0.5) return acc + c * 0.15;
        c += (c + 0.015) * acc * 1.2;
    }
    vec3 cam = (u_inv_view * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    vec3 ray = (u_inv_view * vec4(eye, 1.0)).xyz - cam;
    float dist = length(ray);
    vec3 dir = ray / max(dist, 1e-3);
    vec3 fc = fog_colour(dir);
    if (sea && u_water.y > 0.5 && dir.z < -1e-4) {
        // the sea's surface along this ray (the depth here is whatever is under the sea)
        dist = min((u_water.x - cam.z) / dir.z, u_nearfar.w * 2.0);
        sky = false;
        d = 0.0;
    }
    if (sky || (d > u_ranges.x && z > u_nearfar.w * 0.97)) {
        // the sky (or past the far city's end): near the horizon as much fog colour as the far city gets
        if (u_sky_haze.x > 0.0) {
            vec2 f = fog_amounts(u_nearfar.w, dir, cam);
            float band = pow(1.0 - smoothstep(-0.05, u_sky_haze.y, dir.z), 1.2);
            c = mix(c, fc, clamp(max(f.x * band, f.y), 0.0, 0.95) * u_sky_haze.x);
        }
        return c;
    }
    vec2 f = fog_amounts(dist, dir, cam) * (u_stage.z > 0.5 ? u_haze.w : 1.0);
    c = mix(c, fc, f.x);
    c = mix(c, mix(fc, u_col_ground, 0.6), f.y);
    return c;
}
vec3 finish(vec3 c) {
    c += texture(u_bloom, v_uv).rgb * u_bloom_on * u_bloom_intensity;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = max(mix(vec3(l), c, u_grade.x), 0.0);
    return c * mix(u_tint_lo, u_tint_hi, smoothstep(0.08, 0.75, l));
}
void main() {
    vec3 c = texture(u_scene, v_uv).rgb;
    if (u_stage.x > 0.5) c = atmosphere(c);
    if (u_stage.y > 0.5) c = finish(c);
    o_color = vec4(c, 1.0);
}
)";

// ---- dynamic lights (5C.6, rebuilt after the owner's feedback) ------------------------------------
// Light data: one column per light in an RGBA32F texture (kLightRows rows):
//   0: position (world), reach      1: colour (r < 0: take it from the atlas), intensity
//   2: atlas rectangle u0 v0 u1 v1  3: direction (world, spot lights), type (0 pool, 1 spot, 2 point)
//   4: bounds centre (world), half size across  5: corona: world size, on (1), fade; half height of the bounds (0 = as across)
// Each light is drawn as a screen rectangle around its bounding sphere (instanced), adding its light
// into an accumulation texture that the composite multiplies the scene by. Cost follows the pixels
// a light covers, so hundreds of lights stay cheap.
const char* kLightCommon = R"(
uniform sampler2D u_ldata;
uniform sampler2D u_atlas;
uniform float u_has_atlas;
uniform vec4 u_ranges;     // split
uniform vec4 u_nearfar;    // near n, near f, far n, far f
uniform vec2 u_tan;        // tan(fov/2) x, y
float eye_z(float d) {
    float split = u_ranges.x;
    float n, f, ndc;
    if (d <= split) { n = u_nearfar.x; f = u_nearfar.y; ndc = d / split * 2.0 - 1.0; }
    else { n = u_nearfar.z; f = u_nearfar.w; ndc = (d - split) / (1.0 - split) * 2.0 - 1.0; }
    float m10 = (f + n) / (f - n), m14 = -2.0 * f * n / (f - n);
    return m14 / (ndc - m10);
}
vec4 ldata(int i, int row) { return texelFetch(u_ldata, ivec2(i, row), 0); }
// A light's colour: given, or the average of its glow sprite in the atlas, scaled to full brightness.
vec3 light_colour(int i) {
    vec4 C = ldata(i, 1);
    if (C.r >= 0.0) return C.rgb;
    vec3 warm = vec3(1.0, 0.86, 0.66);
    if (u_has_atlas < 0.5) return warm;
    vec4 U = ldata(i, 2);
    vec3 s = vec3(0.0);
    for (int k = 0; k < 9; ++k) {
        vec2 f = vec2(float(k - (k / 3) * 3), float(k / 3)) * 0.25 + 0.25;
        s += textureLod(u_atlas, mix(U.xy, U.zw, f), 1.0).rgb;
    }
    float m = max(s.r, max(s.g, s.b));
    return m < 0.05 ? warm : s / m;
}
)";

const char* kLightVS = R"(
uniform mat4 u_view;       // world -> eye
uniform float u_near;
flat out int v_id;
flat out vec3 v_col;
out vec2 v_uv;
void main() {
    int i = gl_InstanceID;
    v_id = i;
    v_col = light_colour(i) * ldata(i, 1).a;
    vec4 B = ldata(i, 4);
    float hz = ldata(i, 5).w;
    vec3 ext = vec3(B.w, B.w, hz > 0.0 ? hz : B.w);  // world box half size (lamp pools are tall: height counts less)
    mat3 R = mat3(u_view);
    vec3 ee = abs(R[0]) * ext.x + abs(R[1]) * ext.y + abs(R[2]) * ext.z;  // the box in eye space
    vec3 e = (u_view * vec4(B.xyz, 1.0)).xyz;
    vec2 lo = vec2(-1.0), hi = vec2(1.0);
    if (e.z + ee.z < u_near) {
        lo = hi = vec2(2.0);  // behind the camera: nothing to draw
    } else if (e.z - ee.z > u_near * 2.0) {
        // conservative screen rectangle of the box: its x / y extent at the nearest and farthest depth
        vec2 a = (e.xy - ee.xy) / u_tan, b = (e.xy + ee.xy) / u_tan;
        float zn = e.z - ee.z, zf = e.z + ee.z;
        lo = max(min(a / zn, a / zf), vec2(-1.0));
        hi = min(max(b / zn, b / zf), vec2(1.0));
    }
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vec2 ndc = mix(lo, hi, corner);
    v_uv = ndc * 0.5 + 0.5;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)";

const char* kLightFS = R"(
uniform sampler2D u_depth;
uniform mat4 u_inv_view;   // eye -> world
uniform vec4 u_spot;       // spot: tan(half width), tan(half height); pool: height weight; -
flat in int v_id;
flat in vec3 v_col;
in vec2 v_uv;
out vec4 o_color;
void main() {
    float d = texture(u_depth, v_uv).r;
    if (d >= 0.99999) discard;
    float z = eye_z(d);
    if (d > u_ranges.x && z > u_nearfar.w * 0.97) discard;  // the sky
    vec3 eye = vec3((v_uv * 2.0 - 1.0) * u_tan * z, z);
    vec3 wp = (u_inv_view * vec4(eye, 1.0)).xyz;
    vec4 P = ldata(v_id, 0);
    vec4 D = ldata(v_id, 3);
    vec3 L = wp - P.xyz;
    float a;
    if (D.w < 0.5) {
        // street light pool: the height difference counts less, so a lamp up on its pole lights the
        // street below it (and the walls and people around)
        vec3 dv = vec3(L.xy, L.z * (L.z < 0.0 ? u_spot.z : 1.0));
        float q = dot(dv, dv) / (P.w * P.w);
        if (q >= 1.0) discard;
        a = (1.0 - q) * (1.0 - q) * smoothstep(0.0, 0.04, q);
    } else if (D.w < 1.5) {
        // headlight: a wide, flat cone along the car's beam, lighting the road ahead
        float fz = dot(L, D.xyz);
        if (fz <= 30.0) discard;
        vec3 rt = normalize(cross(D.xyz, vec3(0.0, 0.0, 1.0)));
        vec3 up = cross(rt, D.xyz);
        vec2 t = vec2(dot(L, rt), dot(L, up)) / (fz * u_spot.xy);
        float cone = dot(t, t);
        float q = length(L) / P.w;
        if (cone >= 1.0 || q >= 1.0) discard;
        a = (1.0 - smoothstep(0.25, 1.0, cone)) * (1.0 - q * q) * (1.0 - q * q) * smoothstep(30.0, 300.0, fz);
    } else {
        // small point light (car flares, muzzle flashes): not on the lamp / car itself
        float q = dot(L, L) / (P.w * P.w);
        if (q >= 1.0) discard;
        a = (1.0 - q) * (1.0 - q) * smoothstep(0.02, 0.2, q);
    }
    o_color = vec4(v_col * a, 1.0);
}
)";

// Coronas: lamps the game no longer draws a glow for (too far away, or culled) still show as a soft
// point of light, at least a few pixels across, hidden behind whatever is in front of them.
const char* kCoronaVS = R"(
uniform mat4 u_view;
uniform float u_near;
uniform sampler2D u_depth;
uniform vec4 u_corona;     // min half size (px), intensity, screen width, screen height
uniform vec4 u_haze;       // the composite's haze by distance (exponent scale, 1 / far end, power)
flat out vec3 v_col;
out vec2 v_q;
void main() {
    int i = gl_InstanceID;
    vec4 F = ldata(i, 5);
    vec4 P = ldata(i, 0);
    vec3 e = (u_view * vec4(P.xyz, 1.0)).xyz;
    vec2 ndc = e.xy / (max(e.z, 1.0) * u_tan);
    float vis = 0.0;
    // only for distant lamps (near ones light the street instead): fades in from 30 to 50 m
    float far_k = smoothstep(3000.0, 5000.0, e.z);
    if (F.y > 0.5 && far_k > 0.0 && abs(ndc.x) < 1.05 && abs(ndc.y) < 1.05) {
        // occlusion: the scene's depth at 5 points around the light, against the light's own
        vec2 c = ndc * 0.5 + 0.5;
        vec2 px = 2.0 / u_corona.zw;
        for (int k = 0; k < 5; ++k) {
            vec2 o = k == 0 ? vec2(0.0) : vec2(float((k & 1) * 2 - 1), float((k >> 1) * 2 - 1)) * px;
            float d = textureLod(u_depth, c + o, 0.0).r;
            float zs = d >= 0.99999 ? 1e9 : eye_z(d);
            vis += zs > e.z - 120.0 ? 0.2 : 0.0;
        }
    }
    float half_px = clamp(F.x / (e.z * u_tan.y) * 0.5 * u_corona.w, u_corona.x, u_corona.x * 8.0);
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
    v_q = corner;
    float haze = u_haze.x > 0.0 ? 1.0 - exp(-u_haze.x * pow(max(e.z, 0.0) * u_haze.y, u_haze.z)) : 0.0;
    v_col = light_colour(i) * ldata(i, 1).a * vis * F.z * far_k * u_corona.y * (1.0 - haze);
    gl_Position = vis > 0.0 ? vec4(ndc + corner * half_px * 2.0 / u_corona.zw, 0.0, 1.0) : vec4(2.0, 2.0, 0.0, 1.0);
}
)";

const char* kCoronaFS = R"(
flat in vec3 v_col;
in vec2 v_q;
out vec4 o_color;
void main() {
    float r2 = dot(v_q, v_q);
    if (r2 >= 1.0) discard;
    float a = exp(-r2 * 5.0) - exp(-5.0);
    o_color = vec4(v_col * a, 1.0);
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

GLuint link2(const std::string& vs_src, const std::string& fs_src, const char* name) {
    const GLuint vs = compile(GL_VERTEX_SHADER, vs_src.c_str(), name), fs = compile(GL_FRAGMENT_SHADER, fs_src.c_str(), name);
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

using glmath::invert4;

// Ground fog parameters (world units are ~1 cm: the player is ~180 tall).
constexpr float kFogFalloff = 1.0f / 1500.0f;   // density halves every ~10 m of height
constexpr float kFogDensity = 1.2e-5f;          // ~30% extra haze over 300 m at the reference height
constexpr float kFogReference = 250.0f;         // reference height: just above the street (Ocean Drive is at ~200, the sea lower)

// Muzzle flashes (world space), added by the game hooks for the player's shots.
struct Flash { float p[3]; Uint32 t; };
Flash g_flashes[4] = {};
int g_flash_next = 0;

// How dark the scene is (0 day .. 1 night), from the time-of-day fog colour: lights should glow a
// lot at night while the bright daytime sky (as bright as a neon tube) should only glow a little.
float g_night = 0;

// ---- dynamic lights: the light list (CPU) ----------------------------------------------------------
constexpr int kMaxLightCols = 256;  // light data texture width
constexpr int kLightRows = 6;
GLuint g_light_prog = 0, g_corona_prog = 0;
GLuint g_ldata_ring[3] = {};        // RGBA32F kMaxLightCols x kLightRows, a new one each frame (no GPU wait)
GLuint g_ldata_tex = 0;             // this frame's
Target g_light_acc;                 // accumulated light (RGBA16F, scene size)
float g_ldata[kLightRows][kMaxLightCols][4];
int g_light_n = 0;                  // lights packed this frame
bool g_any_corona = false;
float g_corona_haze[3] = {0, 1, 1};  // the composite's haze (set with its uniforms) for the coronas
GLuint g_atlas = 0;                 // the glow atlas the light colours are read from

// City lamps and neon: the game only draws their glow billboards while they are in its range and
// their grid cell is in view, so a light seen once is remembered in world space (merged per ~1.5 m)
// and keeps lighting the street when the camera gets close, looks away or the glow is out of range.
struct StaticLight {
    float pos[3];
    float size;
    float uv[4];
    unsigned tex;
    Uint32 first_seen, last_seen;
    int hits;
    float corona;  // 0..1 fade of the corona (shown while the game draws no glow here)
    float fade;    // 0..1: fades in when it is confirmed (second sighting) and out when it is dropped
    bool dying;    // dropped: fading out, removed at 0 (seen again: back)
};
constexpr float kPoolHeight = 0.3f;  // lamp pools: the height difference counts 30%, so the street below is lit
constexpr float kCell = 300.0f;  // glows closer than this are one light (a neon tube is many quads)
std::unordered_map<int64_t, StaticLight> g_static;
float g_glow_range = 0;             // how far the game draws city glows (recent maximum)
int64_t cell_key(int x, int y, int z) {
    return (int64_t(x & 0x1FFFFF) << 42) | (int64_t(y & 0x1FFFFF) << 21) | int64_t(z & 0x1FFFFF);
}
// Car lights: per frame from the glow draws with a model matrix. A light whose glow is missing is
// kept a while, moving on at its speed: off screen the game culls the car (and its glows) while its
// beam may still light the road in view; in view a flare can skip frames. Fades in and out.
struct DynLight {
    int type;          // 1 spot (headlight beam), 2 point (flare)
    float pos[3], dir[3];
    float vel[3];      // world units / s (smoothed)
    float size;
    float uv[4];
    unsigned tex;
    float alpha;
    Uint32 last_seen;
    bool matched;
};
std::vector<DynLight> g_dyn;
// The light-effects atlas: the texture most glow quads use, voted over many frames. Only its sprites
// make lights; other additive draws (mission markers, muzzle flash sprites, explosions) do not.
std::unordered_map<unsigned, float> g_atlas_votes;

void xform(const float* m, const float* v, float* out) {  // column-major 4x4 * (v, 1)
    for (int i = 0; i < 3; ++i) out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i];
}
void xform_dir(const float* m, const float* v, float* out) {
    for (int i = 0; i < 3; ++i) out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2];
}
float dist3(const float* a, const float* b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

void update_light_memory(const Scene& sc, const float* cam, Uint32 now, float dt) {
    // the atlas: votes decay over a few seconds
    for (auto& [t, v] : g_atlas_votes) v *= 0.97f;
    for (int i = 0; i < sc.glow_count; ++i) g_atlas_votes[sc.glows[i].tex] += 1.0f;
    float votes = 0;
    for (const auto& [t, v] : g_atlas_votes)
        if (v > votes) votes = v, g_atlas = t;
    // static: merge this frame's city glows
    float frame_range = 0;
    for (int i = 0; i < sc.glow_count; ++i) {
        const Scene::Glow& g = sc.glows[i];
        if (!g.world || g.tex != g_atlas) continue;
        const float* w = g.pos;
        frame_range = std::max(frame_range, dist3(w, cam));
        const int cx = int(std::floor(w[0] / kCell)), cy = int(std::floor(w[1] / kCell)), cz = int(std::floor(w[2] / kCell));
        StaticLight* hit = nullptr;
        if (auto own = g_static.find(cell_key(cx, cy, cz)); own != g_static.end() && dist3(own->second.pos, w) < kCell)
            hit = &own->second;
        for (int dx = -1; dx <= 1 && !hit; ++dx)
            for (int dy = -1; dy <= 1 && !hit; ++dy)
                for (int dz = -1; dz <= 1 && !hit; ++dz) {
                    auto it = g_static.find(cell_key(cx + dx, cy + dy, cz + dz));
                    if (it != g_static.end() && dist3(it->second.pos, w) < kCell) hit = &it->second;
                }
        if (hit) {
            hit->size = std::max(hit->size, g.size);
            hit->last_seen = now;
            hit->dying = false;
            ++hit->hits;
        } else {
            StaticLight L{};
            std::copy(w, w + 3, L.pos);
            L.size = g.size;
            std::copy(g.uv, g.uv + 4, L.uv);
            L.tex = g.tex;
            L.first_seen = L.last_seen = now;
            L.hits = 1;
            g_static.emplace(cell_key(cx, cy, cz), L);
        }
    }
    g_glow_range = std::max(g_glow_range * std::pow(0.5f, dt), frame_range);
    // forget: far away, a one-off, or in view and in the game's range but no longer drawn (switched off)
    const float keep = g_set.light_distance * 100.0f * 1.25f;
    for (auto it = g_static.begin(); it != g_static.end();) {
        StaticLight& L = it->second;
        const float d = dist3(L.pos, cam);
        const Uint32 unseen = now - L.last_seen;
        bool drop = d > keep || (L.hits < 3 && unseen > 2000);
        if (!drop && unseen > 1500 && d > 2500 && d < 0.8f * g_glow_range) {
            float e[3];
            xform(sc.view, L.pos, e);
            drop = e[2] > 0 && std::fabs(e[0]) < e[2] * sc.tan_half_x * 0.9f && std::fabs(e[1]) < e[2] * sc.tan_half_y * 0.9f;
        }
        // corona while the game draws no glow here (fades in / out)
        const float target = unseen > 250 ? 1.0f : 0.0f;
        L.corona += std::clamp(target - L.corona, -dt / 0.2f, dt / 0.4f);
        // fade in once confirmed, out when dropped (a one-off never showed: gone at once)
        if (drop && L.hits >= 2) L.dying = true;
        if (L.dying) L.fade -= dt / 0.5f;
        else if (L.hits >= 2) L.fade = std::min(1.0f, L.fade + dt / 0.4f);
        const bool gone = (drop && L.hits < 2) || (L.dying && L.fade <= 0) || d > keep;
        it = gone ? g_static.erase(it) : std::next(it);
    }

    // dynamic: group the car-light quads by draw
    struct Group {
        float c[3] = {}, origin[3] = {};
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        float size = 0;
        int quads = 0, first = -1;
    };
    std::vector<Group> groups;
    int last_draw = -1;
    for (int i = 0; i < sc.glow_count; ++i) {
        const Scene::Glow& g = sc.glows[i];
        if (g.world || g.tex != g_atlas) continue;
        if (g.draw != last_draw) {
            groups.push_back({});
            groups.back().first = i;
            std::copy(g.origin, g.origin + 3, groups.back().origin);
            last_draw = g.draw;
        }
        Group& G = groups.back();
        for (int k = 0; k < 3; ++k) {
            G.c[k] += g.pos[k];
            G.lo[k] = std::min(G.lo[k], g.pos[k] - g.size * 0.5f);
            G.hi[k] = std::max(G.hi[k], g.pos[k] + g.size * 0.5f);
        }
        G.size = std::max(G.size, g.size);
        ++G.quads;
    }
    for (DynLight& D : g_dyn) D.matched = false;
    for (Group& G : groups) {
        for (float& v : G.c) v /= float(G.quads);
        const Scene::Glow& g0 = sc.glows[G.first];
        DynLight n{};
        const float off[3] = {G.c[0] - G.origin[0], G.c[1] - G.origin[1], G.c[2] - G.origin[2]};
        const float offl = std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
        const float extent = std::max({G.hi[0] - G.lo[0], G.hi[1] - G.lo[1], G.hi[2] - G.lo[2]});
        if (G.quads >= 4 && offl > 0.15f * extent) {
            // a headlight beam: from its mount point along the beam, aimed a little down at the road
            n.type = 1;
            std::copy(G.origin, G.origin + 3, n.pos);
            float d[3] = {off[0], off[1], off[2]};
            const float horiz = std::sqrt(d[0] * d[0] + d[1] * d[1]);
            if (horiz < 1e-3f) continue;
            d[0] /= horiz, d[1] /= horiz;
            d[2] = -0.06f;  // level beam, a few degrees down
            const float h = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (int k = 0; k < 3; ++k) n.dir[k] = d[k] / h;
        } else {
            n.type = 2;
            std::copy(G.c, G.c + 3, n.pos);
        }
        n.size = G.size;
        std::copy(g0.uv, g0.uv + 4, n.uv);
        n.tex = g0.tex;
        DynLight* best = nullptr;
        float bd = 250.0f;
        for (DynLight& D : g_dyn)
            if (!D.matched && D.type == n.type && dist3(D.pos, n.pos) < bd) bd = dist3(D.pos, n.pos), best = &D;
        n.last_seen = now;
        if (best) {
            n.alpha = std::min(1.0f, best->alpha + dt / 0.15f);
            const float k = std::min(1.0f, dt / 0.15f);  // smoothed speed
            for (int j = 0; j < 3; ++j)
                n.vel[j] = best->vel[j] + ((n.pos[j] - best->pos[j]) / std::max(dt, 1e-3f) - best->vel[j]) * k;
            *best = n;
        } else {
            n.alpha = std::min(1.0f, dt / 0.15f);
            g_dyn.push_back(n);
            best = &g_dyn.back();
        }
        best->matched = true;
    }
    // unmatched: kept moving at its speed for a while (longer off screen, where the game culls the car)
    const float sx = std::sqrt(1 + sc.tan_half_x * sc.tan_half_x), sy = std::sqrt(1 + sc.tan_half_y * sc.tan_half_y);
    for (DynLight& D : g_dyn) {
        if (D.matched) continue;
        float e[3];
        xform(sc.view, D.pos, e);
        const float r = 150.0f;
        const bool in_view = e[2] + r > 0 && std::fabs(e[0]) - r * sx < e[2] * sc.tan_half_x &&
                             std::fabs(e[1]) - r * sy < e[2] * sc.tan_half_y;
        const Uint32 hold = in_view ? 300 : 1500;
        for (int j = 0; j < 3; ++j) D.pos[j] += D.vel[j] * dt;
        if (now - D.last_seen > hold) D.alpha -= dt / 0.3f;
    }
    g_dyn.erase(std::remove_if(g_dyn.begin(), g_dyn.end(), [](const DynLight& D) { return D.alpha <= 0; }), g_dyn.end());
}

// Packs the lights for this frame: the strongest on screen first, up to max_lights.
void pack_lights(const Scene& sc, const float* cam, Uint32 now) {
    struct Cand { int kind; int index; float score; };  // kind 0 static, 1 car light, 2 muzzle flash
    std::vector<Cand> cand;
    std::vector<const StaticLight*> statics;
    statics.reserve(g_static.size());
    const float night_static = std::clamp((g_night - 0.15f) / 0.45f, 0.0f, 1.0f);
    const float night_dyn = 0.25f + 0.75f * std::clamp((g_night - 0.1f) / 0.5f, 0.0f, 1.0f);
    const float max_d = g_set.light_distance * 100.0f;
    const float sx = std::sqrt(1 + sc.tan_half_x * sc.tan_half_x), sy = std::sqrt(1 + sc.tan_half_y * sc.tan_half_y);
    auto visible = [&](const float* center, float r) {  // bounding sphere against the view frustum
        float e[3];
        xform(sc.view, center, e);
        if (e[2] + r < 0) return false;
        return std::fabs(e[0]) - r * sx < e[2] * sc.tan_half_x && std::fabs(e[1]) - r * sy < e[2] * sc.tan_half_y;
    };
    auto pool_radius = [&](float size) {
        return std::clamp(450.0f + size * 1.0f, 450.0f, 1000.0f) * std::clamp(g_set.street_light_range, 0.25f, 4.0f);
    };
    if (night_static > 0 && g_set.street_lights > 0)
        for (const auto& kv : g_static) {
            const StaticLight& L = kv.second;
            const float d = dist3(L.pos, cam);
            if (d > max_d || L.hits < 2 || L.fade <= 0.01f) continue;
            const float r = pool_radius(L.size);
            if (!visible(L.pos, r / kPoolHeight)) continue;
            statics.push_back(&L);
            cand.push_back({0, int(statics.size()) - 1, g_set.street_lights * r / std::max(d - r, 100.0f)});
        }
    const float tanw = std::tan(std::clamp(g_set.headlight_width, 10.0f, 150.0f) * 0.5f * 3.14159265f / 180.0f);
    for (int i = 0; i < int(g_dyn.size()); ++i) {
        const DynLight& D = g_dyn[size_t(i)];
        if (D.type == 1 && g_set.headlights <= 0) continue;
        const float range = D.type == 1 ? g_set.headlight_range * 100.0f : std::clamp(D.size * 1.5f, 150.0f, 400.0f);
        float c[3] = {D.pos[0], D.pos[1], D.pos[2]};
        float r = range;
        if (D.type == 1) {
            for (int k = 0; k < 3; ++k) c[k] += D.dir[k] * range * 0.5f;
            r = range * std::sqrt(0.25f + tanw * tanw);
        }
        if (dist3(D.pos, cam) > max_d || !visible(c, r)) continue;
        cand.push_back({1, i, 3.0f * range / std::max(dist3(c, cam) - r, 100.0f)});  // car lights first
    }
    if (g_set.muzzle_flash > 0)
        for (int i = 0; i < 4; ++i)
            if (g_flashes[i].t && now - g_flashes[i].t < 60) cand.push_back({2, i, 1e6f});
    std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    const int n = std::min<int>(int(cand.size()), std::clamp(g_set.max_lights, 8, kMaxLightCols));
    g_any_corona = false;
    for (int j = 0; j < n; ++j) {
        float* row[kLightRows];
        for (int r = 0; r < kLightRows; ++r) {
            row[r] = g_ldata[r][j];
            std::fill(row[r], row[r] + 4, 0.0f);
        }
        const Cand& c = cand[size_t(j)];
        if (c.kind == 0) {
            const StaticLight& L = *statics[size_t(c.index)];
            const float r = pool_radius(L.size);
            std::copy(L.pos, L.pos + 3, row[0]);
            row[0][3] = r;
            row[1][0] = -1;
            row[1][3] = 0.45f * g_set.street_lights * night_static * L.fade;
            std::copy(L.uv, L.uv + 4, row[2]);
            row[3][3] = 0;
            std::copy(L.pos, L.pos + 3, row[4]);
            // bounds: across r; down to the street (height counts kPoolHeight, at most 15 m), up only r
            const float down = std::min(r / kPoolHeight, 1500.0f);
            row[4][2] = L.pos[2] + (r - down) * 0.5f;
            row[4][3] = r;
            row[5][3] = (r + down) * 0.5f;
            if (g_set.light_coronas && L.corona > 0.01f) {
                row[5][0] = std::max(L.size * 1.2f, 120.0f);
                row[5][1] = 1;
                row[5][2] = L.corona;
                g_any_corona = true;
            }
        } else if (c.kind == 1) {
            const DynLight& D = g_dyn[size_t(c.index)];
            std::copy(D.pos, D.pos + 3, row[0]);
            std::copy(D.uv, D.uv + 4, row[2]);
            row[1][0] = -1;
            if (D.type == 1) {
                const float range = g_set.headlight_range * 100.0f;
                row[0][3] = range;
                row[1][3] = g_set.headlights * 1.3f * night_dyn * D.alpha;
                std::copy(D.dir, D.dir + 3, row[3]);
                row[3][3] = 1;
                for (int k = 0; k < 3; ++k) row[4][k] = D.pos[k] + D.dir[k] * range * 0.5f;
                row[4][3] = range * std::sqrt(0.25f + tanw * tanw);
            } else {
                const float r = std::clamp(D.size * 1.5f, 150.0f, 400.0f);
                row[0][3] = r;
                row[1][3] = 0.5f * night_dyn * D.alpha;
                row[3][3] = 2;
                std::copy(D.pos, D.pos + 3, row[4]);
                row[4][3] = r;
            }
        } else {
            // a short warm flicker around the shooter: faint by day (the light multiplies the scene, so a
            // full one doubled a sunlit street), stronger at night; the owner found 1.0 / 7.5 m too bright
            const Flash& f = g_flashes[c.index];
            std::copy(f.p, f.p + 3, row[0]);
            row[0][3] = 550;
            row[1][0] = 1.0f, row[1][1] = 0.72f, row[1][2] = 0.38f;
            row[1][3] = std::clamp(g_set.muzzle_flash, 0.0f, 1.0f) * (0.25f + 0.75f * g_night);
            row[3][3] = 2;
            std::copy(f.p, f.p + 3, row[4]);
            row[4][3] = 550;
        }
    }
    g_light_n = n;
}

bool ensure_light_target(int w, int h) {
    if (g_light_acc.w == w && g_light_acc.h == h && g_light_acc.fbo) return true;
    free_target(g_light_acc);
    if (!g_light_acc.fbo) p_glGenFramebuffers(1, &g_light_acc.fbo);
    g_light_acc.color = make_tex(w, h, GL_RGBA16F, GL_RGBA, GL_FLOAT, GL_LINEAR);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_light_acc.fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_light_acc.color, 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG("post: light target %dx%d incomplete", w, h);
        free_target(g_light_acc);
        return false;
    }
    g_light_acc.w = w;
    g_light_acc.h = h;
    return true;
}

// Shared uniforms of the light and corona programs.
void light_uniforms(GLuint p, const Scene& sc, const float* inv_view) {
    p_glUniform1i(uni(p, "u_ldata"), 5);
    p_glUniform1i(uni(p, "u_atlas"), 4);
    p_glUniform1i(uni(p, "u_depth"), 2);
    p_glUniform1f(uni(p, "u_has_atlas"), g_atlas ? 1.0f : 0.0f);
    p_glUniformMatrix4fv(uni(p, "u_view"), 1, GL_FALSE, sc.view);
    p_glUniformMatrix4fv(uni(p, "u_inv_view"), 1, GL_FALSE, inv_view);
    p_glUniform2f(uni(p, "u_tan"), sc.tan_half_x, sc.tan_half_y);
    p_glUniform1f(uni(p, "u_near"), sc.near_n);
    p_glUniform4f(uni(p, "u_ranges"), sc.split, 0, 0, 0);
    p_glUniform4f(uni(p, "u_nearfar"), sc.near_n, sc.near_f, sc.far_n, sc.far_f);
}

// Binds the light data (unit 5, uploaded), the atlas (4) and the scene depth (2).
void bind_light_inputs() {
    static int ring = 0;
    g_ldata_tex = g_ldata_ring[ring++ % 3];
    p_glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, g_ldata_tex);
    for (int r = 0; r < kLightRows; ++r)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, r, g_light_n, 1, GL_RGBA, GL_FLOAT, &g_ldata[r][0][0]);
    p_glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, g_atlas);
    p_glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_scene.depth);
    p_glActiveTexture(GL_TEXTURE0);
}

void run_light_pass(const Scene& sc, const float* inv_view) {
    bind_light_inputs();
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_light_acc.fbo);
    glViewport(0, 0, g_light_acc.w, g_light_acc.h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    p_glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    const GLuint p = g_light_prog;
    p_glUseProgram(p);
    light_uniforms(p, sc, inv_view);
    const float tanw = std::tan(std::clamp(g_set.headlight_width, 10.0f, 150.0f) * 0.5f * 3.14159265f / 180.0f);
    p_glUniform4f(uni(p, "u_spot"), tanw, tanw * 0.32f, kPoolHeight, 0.0f);
    p_glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, g_light_n);
    glDisable(GL_BLEND);
}

void run_corona_pass(const Scene& sc, const float* inv_view) {
    glEnable(GL_BLEND);
    p_glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
    const GLuint p = g_corona_prog;
    p_glUseProgram(p);
    light_uniforms(p, sc, inv_view);
    p_glUniform4f(uni(p, "u_corona"), std::max(1.5f, sc.h / 480.0f * 1.5f),
                  0.9f * std::clamp(g_set.street_lights, 0.0f, 3.0f), float(sc.w), float(sc.h));
    p_glUniform4f(uni(p, "u_haze"), g_corona_haze[0], g_corona_haze[1], g_corona_haze[2], 0.0f);
    p_glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, g_light_n);
    glDisable(GL_BLEND);
}

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
#define LOAD(type, name) GLAPI_LOAD(type, name, ok)
    POST_GL_FUNCS(LOAD)
#undef LOAD
    if (!ok) return false;
    g_test_prog = link(kTestFS, "test.fs");
    g_bright_prog = link(kBrightFS, "bright.fs");
    g_down_prog = link(kDownFS, "down.fs");
    g_up_prog = link(kUpFS, "up.fs");
    g_composite_prog = link(kCompositeFS, "composite.fs");
    g_light_prog = link2(std::string(kLightCommon) + kLightVS, std::string(kLightCommon) + kLightFS, "lights");
    g_corona_prog = link2(std::string(kLightCommon) + kCoronaVS, std::string(kLightCommon) + kCoronaFS, "coronas");
    if (g_light_prog) {
        glGenTextures(3, g_ldata_ring);
        for (GLuint t : g_ldata_ring) {
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kMaxLightCols, kLightRows, 0, GL_RGBA, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    g_ok = g_test_prog && g_bright_prog && g_down_prog && g_up_prog && g_composite_prog;
    if (!g_light_prog) LOG("post: dynamic lights disabled (shader errors)");
    LOG("post: %s", g_ok ? "ready" : "disabled (shader errors)");
    return g_ok;
}

void set_settings(const Settings& s) { g_set = s; }

namespace {
bool g_player_valid = false;
float g_player[3] = {};
}
void set_player(bool valid, const float pos[3]) {
    g_player_valid = valid;
    if (valid) std::memcpy(g_player, pos, sizeof g_player);
}

void flash(float x, float y, float z) {
    Flash& f = g_flashes[g_flash_next++ % 4];
    f.p[0] = x;
    f.p[1] = y;
    f.p[2] = z;
    f.t = std::max<Uint32>(1, SDL_GetTicks());
}

namespace {
float g_sun_dir[3] = {-0.97f, 0.0f, 0.25f};
int g_minute = -1;           // game clock (minutes since midnight), -1 = unknown
float g_bloom_tod = -1;      // smoothed time-of-day bloom factor (adaptive bloom)
LateFog g_late;              // GL fog for the draws after the atmosphere stage
// Adaptive bloom: none by day (07:30-16:30), easing in through sunset (to 19:30), full at night,
// easing out through dawn (04:30-07:30).
float bloom_time_factor(int minute) {
    auto ease = [](float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3 - 2 * t); };
    const float h = float(((minute % 1440) + 1440) % 1440) / 60.0f;
    if (h >= 7.5f && h < 16.5f) return 0.0f;
    if (h >= 16.5f && h < 19.5f) return ease((h - 16.5f) / 3.0f);
    if (h >= 4.5f && h < 7.5f) return 1.0f - ease((h - 4.5f) / 3.0f);
    return 1.0f;
}

// ---- the atmosphere by time of day (the Miami palette) -------------------------------------------
// Key times follow the game's own sky (looked at every hour from the beach): sunrise peaks at
// 05:00-05:30, day from 06:30, golden hour 16:00-17:30, sunset 18:00-18:40 going violet, night from
// 19:00. Colours in the Vice City / GTA VI spirit: peach and lavender at sunrise, a pale warm-yellow
// horizon under a blue sky by day, amber then coral and violet at sunset, indigo at night.
struct AtmoKey {
    float hour;
    float horizon[3], zenith[3], sun[3], ground[3];
    float day, golden, night;  // the fog amount: a mix of the three amount settings
    float glow;                // how much of the sun colour toward the sun's side
    float sat;                 // colour grade: saturation, shadow and highlight multipliers
    float lo[3], hi[3];
};
constexpr AtmoKey kAtmo[] = {
    // hour   horizon                zenith                 sun side               ground fog            day  gold night glow  sat    shadows                highlights
    {3.5f,   {0.13f, 0.15f, 0.30f}, {0.06f, 0.08f, 0.20f}, {0.18f, 0.16f, 0.32f}, {0.17f, 0.15f, 0.32f}, 0.0f, 0.0f, 1.0f, 0.2f, 1.00f, {1.00f, 1.00f, 1.00f}, {1.00f, 1.00f, 1.00f}},
    {4.5f,   {0.55f, 0.42f, 0.62f}, {0.30f, 0.30f, 0.55f}, {1.00f, 0.62f, 0.50f}, {0.62f, 0.50f, 0.70f}, 0.0f, 0.6f, 0.4f, 0.5f, 1.15f, {0.92f, 0.92f, 1.10f}, {1.05f, 0.97f, 1.00f}},
    {5.25f,  {1.00f, 0.64f, 0.60f}, {0.62f, 0.55f, 0.85f}, {1.00f, 0.75f, 0.42f}, {1.00f, 0.72f, 0.70f}, 0.0f, 1.0f, 0.0f, 0.7f, 1.30f, {0.95f, 0.88f, 1.08f}, {1.10f, 0.96f, 0.88f}},
    {6.25f,  {1.00f, 0.84f, 0.66f}, {0.66f, 0.80f, 1.00f}, {1.00f, 0.86f, 0.55f}, {0.98f, 0.90f, 0.78f}, 0.5f, 0.5f, 0.0f, 0.5f, 1.15f, {0.97f, 0.96f, 1.04f}, {1.06f, 1.00f, 0.92f}},
    {7.5f,   {0.98f, 0.93f, 0.80f}, {0.60f, 0.78f, 1.00f}, {1.00f, 0.95f, 0.80f}, {0.92f, 0.94f, 0.95f}, 1.0f, 0.0f, 0.0f, 0.3f, 1.08f, {0.97f, 0.99f, 1.04f}, {1.03f, 1.01f, 0.96f}},
    {15.5f,  {0.98f, 0.93f, 0.80f}, {0.60f, 0.78f, 1.00f}, {1.00f, 0.95f, 0.80f}, {0.92f, 0.94f, 0.95f}, 1.0f, 0.0f, 0.0f, 0.3f, 1.08f, {0.97f, 0.99f, 1.04f}, {1.03f, 1.01f, 0.96f}},
    {16.5f,  {1.00f, 0.82f, 0.55f}, {0.62f, 0.72f, 0.95f}, {1.00f, 0.78f, 0.40f}, {1.00f, 0.86f, 0.66f}, 0.4f, 0.6f, 0.0f, 0.5f, 1.15f, {0.96f, 0.96f, 1.05f}, {1.08f, 1.00f, 0.88f}},
    {17.25f, {1.00f, 0.66f, 0.38f}, {0.66f, 0.55f, 0.85f}, {1.00f, 0.58f, 0.25f}, {1.00f, 0.70f, 0.50f}, 0.0f, 1.0f, 0.0f, 0.7f, 1.30f, {0.94f, 0.90f, 1.08f}, {1.12f, 0.98f, 0.82f}},
    {18.0f,  {1.00f, 0.45f, 0.42f}, {0.52f, 0.36f, 0.78f}, {1.00f, 0.48f, 0.20f}, {0.95f, 0.45f, 0.60f}, 0.0f, 1.0f, 0.0f, 0.8f, 1.35f, {0.92f, 0.86f, 1.10f}, {1.12f, 0.94f, 0.86f}},
    {18.67f, {0.66f, 0.32f, 0.60f}, {0.28f, 0.22f, 0.55f}, {0.95f, 0.40f, 0.42f}, {0.55f, 0.30f, 0.58f}, 0.0f, 0.7f, 0.3f, 0.6f, 1.25f, {0.90f, 0.86f, 1.12f}, {1.06f, 0.92f, 1.00f}},
    {19.25f, {0.28f, 0.22f, 0.48f}, {0.10f, 0.12f, 0.32f}, {0.45f, 0.28f, 0.50f}, {0.30f, 0.22f, 0.45f}, 0.0f, 0.25f, 0.75f, 0.4f, 1.10f, {0.95f, 0.94f, 1.06f}, {1.00f, 0.98f, 1.00f}},
    {20.5f,  {0.13f, 0.15f, 0.30f}, {0.06f, 0.08f, 0.20f}, {0.18f, 0.16f, 0.32f}, {0.17f, 0.15f, 0.32f}, 0.0f, 0.0f, 1.0f, 0.2f, 1.00f, {1.00f, 1.00f, 1.00f}, {1.00f, 1.00f, 1.00f}},
};
struct Atmo {
    float horizon[3], zenith[3], sun[3], ground[3];
    float amount, glow, sat, lo[3], hi[3];
};
Atmo atmosphere_now(const Scene& sc) {
    constexpr int n = int(sizeof kAtmo / sizeof kAtmo[0]);
    float h = g_minute < 0 ? 12.0f : float(g_minute % 1440) / 60.0f;
    if (h < kAtmo[0].hour) h += 24.0f;  // before the first key: still the previous night
    int i = n - 1;
    for (int k = 0; k + 1 < n; ++k)
        if (h >= kAtmo[k].hour && h < kAtmo[k + 1].hour) { i = k; break; }
    const AtmoKey& a = kAtmo[i];
    const AtmoKey& b = kAtmo[(i + 1) % n];
    const float span = (i + 1 < n ? b.hour : b.hour + 24.0f) - a.hour;
    float t = std::clamp((h - a.hour) / span, 0.0f, 1.0f);
    t = t * t * (3 - 2 * t);
    auto mix3 = [t](const float* x, const float* y, float* o) { for (int k = 0; k < 3; ++k) o[k] = x[k] + (y[k] - x[k]) * t; };
    auto mix1 = [t](float x, float y) { return x + (y - x) * t; };
    Atmo r{};
    mix3(a.horizon, b.horizon, r.horizon);
    mix3(a.zenith, b.zenith, r.zenith);
    mix3(a.sun, b.sun, r.sun);
    mix3(a.ground, b.ground, r.ground);
    mix3(a.lo, b.lo, r.lo);
    mix3(a.hi, b.hi, r.hi);
    r.amount = mix1(a.day, b.day) * g_set.fog_day + mix1(a.golden, b.golden) * g_set.fog_golden +
               mix1(a.night, b.night) * g_set.fog_night;
    r.glow = mix1(a.glow, b.glow);
    r.sat = mix1(a.sat, b.sat);
    if (!g_set.fog_palette) {  // the game's own (flat) fog colour, warmer toward the sun at sunrise / sunset
        const float warm = std::clamp((sc.fog_color[0] - sc.fog_color[2]) * 4.0f, 0.0f, 1.0f);
        const float sun[3] = {1.0f, 0.62f, 0.32f};
        for (int k = 0; k < 3; ++k) {
            r.horizon[k] = r.zenith[k] = r.ground[k] = sc.fog_color[k];
            r.sun[k] = sc.fog_color[k] + (sun[k] - sc.fog_color[k]) * 0.8f * warm;
        }
    }
    // the grade at its strength setting (0 = none, 1 = as above, 2 = twice as far from neutral)
    const float g = std::clamp(g_set.sunset_colour, 0.0f, 2.0f);
    r.sat = 1.0f + (r.sat - 1.0f) * g;
    for (int k = 0; k < 3; ++k) r.lo[k] = 1.0f + (r.lo[k] - 1.0f) * g, r.hi[k] = 1.0f + (r.hi[k] - 1.0f) * g;
    return r;
}
}  // namespace

void set_time_of_day(int minute) {
    g_minute = minute;
    // Rises in the east (+x) at 05:00, highest in the south around midday, sets in the west (-x) at
    // 18:45 (the game's sky: sunrise peaks at 05:00-05:30, sunset at 18:00-18:40).
    const float k = std::clamp((minute / 60.0f - 5.0f) / 13.75f, -0.1f, 1.1f) * 3.14159265f;
    float d[3] = {std::cos(k), -0.35f * std::sin(k), 0.12f + 0.9f * std::max(0.0f, std::sin(k))};
    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; ++i) g_sun_dir[i] = d[i] / l;
}

bool active() {
    return g_ok && (g_set.test != nullptr || g_set.bloom || g_set.height_fog || g_set.lights || g_set.sky_haze || g_set.fog ||
                    g_set.sunset_colour > 0);
}
bool screen_fog() { return g_ok && g_set.fog; }
LateFog late_fog() { return g_late; }

// The state GLES 3 has that the passes change, saved and restored by hand (the shader renderer and
// Android: no attribute stacks there).
struct SavedState {
    GLint draw_fb = 0, read_fb = 0, prog = 0, array = 0, elem = 0, active = 0, tex[6] = {}, viewport[4] = {};
    GLfloat clear[4] = {};
    GLint blend[4] = {GL_ONE, GL_ZERO, GL_ONE, GL_ZERO};  // src rgb, dst rgb, src alpha, dst alpha
    GLint stencil[7] = {GL_ALWAYS, 0, -1, -1, GL_KEEP, GL_KEEP, GL_KEEP};  // func, ref, value mask, write mask, ops
    GLboolean caps[5] = {}, color_mask[4] = {}, depth_mask = GL_TRUE;
};
constexpr GLenum kSavedCaps[5] = {GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST};

void save_state(SavedState& s) {
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.draw_fb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.read_fb);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.array);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.elem);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.active);
    for (int i = 0; i < 6; ++i) {
        p_glActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex[i]);
    }
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s.clear);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    for (int i = 0; i < 5; ++i) s.caps[i] = glIsEnabled(kSavedCaps[i]);
    glGetBooleanv(GL_COLOR_WRITEMASK, s.color_mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depth_mask);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.blend[0]);
    glGetIntegerv(GL_BLEND_DST_RGB, &s.blend[1]);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.blend[2]);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.blend[3]);
    glGetIntegerv(GL_STENCIL_FUNC, &s.stencil[0]);
    glGetIntegerv(GL_STENCIL_REF, &s.stencil[1]);
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &s.stencil[2]);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &s.stencil[3]);
    glGetIntegerv(GL_STENCIL_FAIL, &s.stencil[4]);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &s.stencil[5]);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &s.stencil[6]);
}

void restore_state(const SavedState& s) {
    p_glUseProgram(GLuint(s.prog));
    for (int i = 0; i < 6; ++i) {
        p_glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, GLuint(s.tex[i]));
    }
    glClearColor(s.clear[0], s.clear[1], s.clear[2], s.clear[3]);
    p_glActiveTexture(GLenum(s.active));
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    for (int i = 0; i < 5; ++i) {
        if (s.caps[i]) glEnable(kSavedCaps[i]);
        else glDisable(kSavedCaps[i]);
    }
    glColorMask(s.color_mask[0], s.color_mask[1], s.color_mask[2], s.color_mask[3]);
    glDepthMask(s.depth_mask);
    p_glBlendFuncSeparate(GLenum(s.blend[0]), GLenum(s.blend[1]), GLenum(s.blend[2]), GLenum(s.blend[3]));
    glStencilFunc(GLenum(s.stencil[0]), s.stencil[1], GLuint(s.stencil[2]));
    glStencilMask(GLuint(s.stencil[3]));
    glStencilOp(GLenum(s.stencil[4]), GLenum(s.stencil[5]), GLenum(s.stencil[6]));
    p_glBindBuffer(GL_ARRAY_BUFFER, GLuint(s.array));
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, GLuint(s.elem));
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(s.draw_fb));
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(s.read_fb));
}

namespace {
// The composite's uniforms for one stage (u_stage.z, the sea pass, is set per draw).
void composite_uniforms(GLuint p, const Scene& sc, const float* inv_view, bool atmo, bool fin, bool lights, bool bloom,
                        float bloom_k, bool light_debug) {
    p_glUniform1i(uni(p, "u_scene"), 0);
    p_glUniform1i(uni(p, "u_bloom"), 1);
    p_glUniform1i(uni(p, "u_depth"), 2);
    p_glUniform1i(uni(p, "u_light_acc"), 3);
    p_glUniform4f(uni(p, "u_stage"), atmo ? 1.0f : 0.0f, fin ? 1.0f : 0.0f, 0.0f, light_debug ? 1.0f : 0.0f);
    p_glUniform4f(uni(p, "u_ranges"), sc.split, 0, 0, 0);
    p_glUniform4f(uni(p, "u_nearfar"), sc.near_n, sc.near_f, sc.far_n, sc.far_f);
    p_glUniform2f(uni(p, "u_tan"), sc.tan_half_x, sc.tan_half_y);
    p_glUniformMatrix4fv(uni(p, "u_inv_view"), 1, GL_FALSE, inv_view);
    const Atmo A = atmosphere_now(sc);
    if (atmo) {
        p_glUniform3f(uni(p, "u_col_horizon"), A.horizon[0], A.horizon[1], A.horizon[2]);
        p_glUniform3f(uni(p, "u_col_zenith"), A.zenith[0], A.zenith[1], A.zenith[2]);
        p_glUniform3f(uni(p, "u_col_sun"), A.sun[0], A.sun[1], A.sun[2]);
        p_glUniform3f(uni(p, "u_col_ground"), A.ground[0], A.ground[1], A.ground[2]);
        p_glUniform3f(uni(p, "u_sun_dir"), g_sun_dir[0], g_sun_dir[1], g_sun_dir[2]);
        p_glUniform4f(uni(p, "u_grad"), 0.30f, 3.0f, A.glow, 0.0f);
        // haze: 1 - exp(-3.5 a (d / far)^1.5): 97% at the far city's end with amount 1, 41% with 0.15
        const float amount = std::clamp(A.amount, 0.0f, 3.0f);
        const float haze_k = g_set.fog ? 3.5f * amount : 0.0f;
        p_glUniform4f(uni(p, "u_haze"), haze_k, 1.0f / std::max(1000.0f, sc.far_f), 1.5f, std::clamp(g_set.water_fog, 0.0f, 1.0f));
        g_corona_haze[0] = haze_k, g_corona_haze[1] = 1.0f / std::max(1000.0f, sc.far_f), g_corona_haze[2] = 1.5f;
        // ground fog: the same time-of-day amount, about the street's height (not the player's: on a
        // roof the street below stays foggy, a jump doesn't move the fog)
        const float ground = g_set.height_fog ? std::clamp(g_set.height_fog_amount, 0.0f, 2.0f) * std::min(amount, 1.5f) : 0.0f;
        p_glUniform4f(uni(p, "u_hf"), kFogDensity, kFogFalloff, kFogReference, ground);
        p_glUniform2f(uni(p, "u_water"), sc.water_z, sc.water_seen ? 1.0f : 0.0f);
        const float far_haze = haze_k > 0 ? 1.0f - std::exp(-haze_k) : 0.0f;
        p_glUniform2f(uni(p, "u_sky_haze"), g_set.sky_haze ? 1.0f : 0.0f, 0.18f + 0.18f * far_haze);
        p_glUniform1f(uni(p, "u_light_on"), lights ? 1.0f : 0.0f);
        p_glUniform1f(uni(p, "u_light_int"), std::clamp(g_set.lights_intensity, 0.0f, 2.0f));
        // what the glows drawn after this stage get: EXP2 equal to the haze at half the far end
        g_late.density = haze_k > 0 ? 2.0f * std::sqrt(1.237f * amount) / std::max(1000.0f, sc.far_f) : 0.0f;
        std::copy(A.horizon, A.horizon + 3, g_late.color);
        static const bool log_atmo = std::getenv("GANGSTAR_POST_LOG") != nullptr;
        static Uint32 logged_at = 0;
        if (log_atmo && SDL_GetTicks() - logged_at > 2000) {
            logged_at = SDL_GetTicks();
            LOG("post: %02d:%02d fog amount %.2f horizon %.2f %.2f %.2f (game fog colour %.2f %.2f %.2f) far end %.0f, "
                "grade %.2f, camera %.0f %.0f %.0f",
                g_minute / 60, g_minute % 60, A.amount, A.horizon[0], A.horizon[1], A.horizon[2], sc.fog_color[0],
                sc.fog_color[1], sc.fog_color[2], sc.far_f, A.sat, inv_view[12], inv_view[13], inv_view[14]);
        }
    }
    if (fin) {
        p_glUniform1f(uni(p, "u_bloom_on"), bloom ? 1.0f : 0.0f);
        // the up chain sums the levels: normalise so intensity means the same at any level count
        p_glUniform1f(uni(p, "u_bloom_intensity"),
                      g_set.bloom_intensity * bloom_k / float(std::max(1, g_level_count)) * 2.0f * (1.0f + 2.0f * g_night));
        p_glUniform4f(uni(p, "u_grade"), A.sat, 0, 0, 0);
        p_glUniform3f(uni(p, "u_tint_lo"), A.lo[0], A.lo[1], A.lo[2]);
        p_glUniform3f(uni(p, "u_tint_hi"), A.hi[0], A.hi[1], A.hi[2]);
    }
}
}  // namespace

void run(const Scene& sc, bool fixed_function, Stage stage) {
    if (!active() || sc.w <= 0 || sc.h <= 0) return;
    SavedState saved;
    save_state(saved);
#ifndef GANGSTAR_GLES
    // Desktop fixed function: the attribute stacks also cover its extra state (fog, alpha test, lighting,
    // texture enables and environments, the client arrays).
    if (fixed_function) {
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
        for (GLenum cap : {GL_ALPHA_TEST, GL_FOG, GL_LIGHTING, GL_COLOR_MATERIAL}) glDisable(cap);
        // the fullscreen triangle comes from gl_VertexID; an enabled array would still be read (stale guest pointers)
        for (GLenum a : {GL_VERTEX_ARRAY, GL_COLOR_ARRAY, GL_NORMAL_ARRAY, GL_TEXTURE_COORD_ARRAY}) glDisableClientState(a);
    }
#endif
    if (!fixed_function)  // the shader renderer's attribute arrays (it re-enables them on its next draw)
        for (GLuint i = 0; i < 8; ++i) p_glDisableVertexAttribArray(i);
    for (GLenum cap : kSavedCaps) glDisable(cap);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glStencilMask(0);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    const bool atmo = stage != Stage::Finish, fin = stage != Stage::Atmosphere;
    if (ensure_scene_target(sc.w, sc.h)) {
        // colour, and the depth where the atmosphere needs it (glows drawn after it write no depth)
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, sc.fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_scene.fbo);
        p_glBlitFramebuffer(0, 0, sc.w, sc.h, 0, 0, sc.w, sc.h, GL_COLOR_BUFFER_BIT | (atmo ? GL_DEPTH_BUFFER_BIT : 0),
                            GL_NEAREST);

        p_glActiveTexture(GL_TEXTURE0);
        const bool light_debug = g_set.test && !std::strcmp(g_set.test, "lights");
        if (g_set.test && !light_debug) {
            if (atmo) {
                p_glBindFramebuffer(GL_FRAMEBUFFER, sc.fbo);
                glViewport(0, 0, sc.w, sc.h);
                p_glUseProgram(g_test_prog);
                p_glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, g_scene.depth);
                p_glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, g_scene.color);
                p_glUniform1i(uni(g_test_prog, "u_scene"), 0);
                p_glUniform1i(uni(g_test_prog, "u_depth"), 1);
                p_glUniform1i(uni(g_test_prog, "u_mode"), !std::strcmp(g_set.test, "invert") ? 1 : !std::strcmp(g_set.test, "sky") ? 3 : 2);
                p_glUniform4f(uni(g_test_prog, "u_ranges"), sc.split, 0, 0, 0);
                p_glUniform4f(uni(g_test_prog, "u_nearfar"), sc.near_n, sc.near_f, sc.far_n, sc.far_f);
                draw_fullscreen();
            }
        } else {
            const float fog_lum = 0.299f * sc.fog_color[0] + 0.587f * sc.fog_color[1] + 0.114f * sc.fog_color[2];
            g_night = std::clamp((0.5f - fog_lum) / 0.35f, 0.0f, 1.0f);
            // adaptive bloom: by time of day, changing slowly (a few seconds) when the clock jumps
            float bloom_k = 1.0f;
            if (fin && g_set.bloom_adaptive) {
                const float target = g_minute < 0 ? 1.0f : bloom_time_factor(g_minute);
                static Uint32 last = 0;
                const Uint32 t = SDL_GetTicks();
                const float step = last ? std::clamp((t - last) / 1000.0f, 0.0f, 0.25f) : 1.0f;
                last = t;
                g_bloom_tod = g_bloom_tod < 0 ? target : g_bloom_tod + (target - g_bloom_tod) * std::min(1.0f, step / 3.0f);
                const float day = std::clamp(g_set.bloom_day, 0.0f, 1.0f);
                bloom_k = day + (1.0f - day) * g_bloom_tod;
            }
            const bool chain = fin && g_set.bloom && g_set.bloom_intensity > 0 && bloom_k > 0.005f &&
                               ensure_bloom_targets(sc.w, sc.h);
            if (chain) run_bloom();
            float inv_view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            const bool have_view = sc.view_valid && invert4(sc.view, inv_view);
            // dynamic lights: remember / pick / draw them into the light buffer
            bool lights = false;
            if (atmo && g_set.lights && g_set.lights_intensity > 0 && have_view && g_light_prog) {
                const Uint32 now = SDL_GetTicks();
                static Uint32 last_ms = 0;
                const float dt = last_ms ? std::clamp((now - last_ms) / 1000.0f, 0.0f, 0.25f) : 0.016f;
                last_ms = now;
                const float cam[3] = {inv_view[12], inv_view[13], inv_view[14]};
                update_light_memory(sc, cam, now, dt);
                pack_lights(sc, cam, now);
                // half resolution: the light is soft, and this is a quarter of the pixels to shade
                lights = g_light_n > 0 && ensure_light_target(std::max(1, sc.w / 2), std::max(1, sc.h / 2));
                if (lights) run_light_pass(sc, inv_view);
                static int logged_lights = 0;
                if (logged_lights++ % 2000 == 300)
                    LOG("post: %d glow quads this frame, %zu remembered lamps, %zu car lights, %d lights drawn (range %.0f)",
                        sc.glow_count, g_static.size(), g_dyn.size(), g_light_n, g_glow_range);
            }
            // composite into the game's framebuffer
            p_glBindFramebuffer(GL_FRAMEBUFFER, sc.fbo);
            glViewport(0, 0, sc.w, sc.h);
            const GLuint p = g_composite_prog;
            p_glUseProgram(p);
            p_glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, chain ? g_levels[0].color : g_scene.color);
            p_glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, g_scene.depth);
            p_glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, lights ? g_light_acc.color : g_scene.color);
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_scene.color);
            // without a camera (menus) there is no atmosphere stage to do
            const bool do_atmo = atmo && have_view;
            composite_uniforms(p, sc, inv_view, do_atmo, fin, lights, chain, bloom_k, light_debug);
            if (do_atmo) {
                // two passes split by the stencil bit the sea's draws set: the sea gets its own share of the fog
                glEnable(GL_STENCIL_TEST);
                glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                glStencilFunc(GL_EQUAL, 0, kWaterStencil);
                draw_fullscreen();
                glStencilFunc(GL_EQUAL, GLint(kWaterStencil), kWaterStencil);
                p_glUniform4f(uni(p, "u_stage"), 1.0f, fin ? 1.0f : 0.0f, 1.0f, light_debug ? 1.0f : 0.0f);
                draw_fullscreen();
                glDisable(GL_STENCIL_TEST);
            } else if (fin) {
                draw_fullscreen();
            }
            if (do_atmo && lights && g_any_corona && g_corona_prog) run_corona_pass(sc, inv_view);
        }
    }

#ifndef GANGSTAR_GLES
    if (fixed_function) {
        glPopClientAttrib();
        glPopAttrib();
    }
#endif
    restore_state(saved);
}

}  // namespace post
