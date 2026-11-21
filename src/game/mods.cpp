#include "game/mods.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>

#include "core/log.h"
#include "hle/overrides.h"
#include "rt/runtime.h"
#include "settings/settings.h"

namespace mods {

namespace {

rt::Runtime* g_rt = nullptr;
uint32_t g_hud = 0;  // CHudManager*
bool g_hidden = false;
bool g_steer_on = false;
float g_steer = 0;
int64_t g_step_ms = 0, g_steps = 0;  // game time steps since the last take_step_stats()
uint32_t g_cam_watch = 0;
bool g_cam_watch_done = false;
std::string g_camera_mode = "classic";  // classic | freeform | first_person
uint32_t g_cam_self = 0;                // FollowCamera being updated (pre -> post)
uint32_t g_cm_self = 0;                 // CameraManager being updated (pre -> post), first person only
float g_cam_look[3] = {}, g_cam_off[3] = {};

// CHudManager fields holding touch controls (from CHudManager::load), with their classes.
struct ControlField { int offset; const char* kind; };
const ControlField kControls[] = {
    {0x28, "AnalogStick"}, {0x2c, "Wheel"},
    {0x30, "VirtualButton"}, {0x34, "VirtualButton"}, {0x38, "VirtualButton"}, {0x3c, "VirtualButton"},
    {0x40, "VirtualButton"}, {0x44, "VirtualButton"}, {0x48, "VirtualButton"}, {0x4c, "VirtualButton"},
    {0x50, "VirtualButton"}, {0x54, "SlideControl"}, {0x58, "SlideControl"}, {0x5c, "VirtualButton"},
    {0x60, "VirtualButton"}, {0x64, "VirtualButton"}, {0x68, "VirtualButton"}, {0x6c, "VirtualButton"},
    {0x70, "VirtualButton"}, {0x74, "ToggleButton"}, {0x7c, "AnimatedButton"}, {0x88, "VirtualButton"},
    {0x8c, "VirtualButton"}, {0x94, "AnimatedButton"},
};

bool is_control(uint32_t obj) {
    if (!g_hud || !obj) return false;
    auto& m = g_rt->mem();
    for (const ControlField& f : kControls)
        if (m.r32(g_hud + f.offset) == obj) return true;
    return false;
}

float f32_at(uint32_t a) {
    float f;
    std::memcpy(&f, g_rt->mem().ptr(a), 4);
    return f;
}

void apply_quality(const settings::Settings& s, cpu::Cpu& c) {
    auto& m = c.rt().mem();
    const uint32_t p = c.rt().lib_object("gPhonePerf");
    auto scale_int = [&](uint32_t off, float k) { m.w32(p + off, uint32_t(int32_t(std::lround(int32_t(m.r32(p + off)) * k)))); };
    const float dd = std::clamp(s.draw_distance, 0.5f, 4.0f);
    scale_int(0x0c, dd);  // texture streaming radius
    scale_int(0x10, dd);  // low-res city far plane
    if (s.quality == "ultra") {
        scale_int(0x1c, 1.5f);  // more peds / traffic
        scale_int(0x20, 1.5f);
        scale_int(0x24, 1.5f);
    }
    m.w8(p + 0x04, s.mipmaps ? 1 : 0);
}

void log_profile(cpu::Cpu& c, const char* when) {
    auto& m = c.rt().mem();
    const uint32_t p = c.rt().lib_object("gPhonePerf");
    char flags[16] = {};
    for (int i = 0; i < 10; ++i) flags[i] = m.r8(p + i) ? '1' : '0';
    LOG("graphics profile (%s): index %d, flags %s, ints %d %d %d %d %d %d %d, flags2 %d %d", when,
        int32_t(m.r32(c.rt().lib_object("_ZN9GS3DStuff12s_perfConfigE"))), flags, int32_t(m.r32(p + 0x0c)),
        int32_t(m.r32(p + 0x10)), int32_t(m.r32(p + 0x14)), int32_t(m.r32(p + 0x18)), int32_t(m.r32(p + 0x1c)),
        int32_t(m.r32(p + 0x20)), int32_t(m.r32(p + 0x24)), m.r8(p + 0x28), m.r8(p + 0x29));
}

}  // namespace

void install(rt::Runtime& rt, const settings::Settings& s) {
    g_rt = &rt;
    g_hidden = s.touch_controls == "hide";

    // ---- time of day override (debugging the day/night look: GANGSTAR_TIME=HH:MM) -------------
    // Pins the game clock through its own DayTime::setTime before every DayTime::update.
    if (const char* t = std::getenv("GANGSTAR_TIME")) {
        int h = 0, m = 0;
        std::sscanf(t, "%d:%d", &h, &m);
        const uint32_t set_time = rt.lib_func("_ZN7DayTime7setTimeEii");
        rt.hook_function("_ZN7DayTime6updateEd", {[set_time, h, m](cpu::Cpu& c) {
                             const uint32_t self = c.reg(cpu::R0);
                             c.call(set_time, {self, uint32_t(h), uint32_t(m)});
                             static int logged_state = -2;
                             auto& mem = c.rt().mem();
                             const int state = int(mem.r32(self + 4));
                             if (state != logged_state) {
                                 auto g = [&](const char* n) { return int(mem.r32(c.rt().lib_object(n))) / 60000; };
                                 LOG("daytime: state %d at %02d:%02d (minutes: dawn %d+%d day %d+%d dusk %d+%d night %d+%d)",
                                     state, h, m, g("_ZN7DayTime9DawnStartE"), g("_ZN7DayTime12DawnDurationE"),
                                     g("_ZN7DayTime8DayStartE"), g("_ZN7DayTime11DayDurationE"), g("_ZN7DayTime9DuskStartE"),
                                     g("_ZN7DayTime12DuskDurationE"), g("_ZN7DayTime10NightStartE"),
                                     g("_ZN7DayTime13NightDurationE"));
                                 logged_state = state;
                             }
                             return false;
                         },
                         nullptr});
        LOG("mods: time of day pinned to %02d:%02d (GANGSTAR_TIME)", h, m);
    }

    // ---- graphics profile ---------------------------------------------------------------------
    const settings::Settings cfg = s;  // captured by value
    rt.hook_function("_ZN9GS3DStuff22loadPerformanceProfileEPKc", {
        [cfg](cpu::Cpu& c) {
            if (cfg.quality != "phone")  // "phone" = exactly what the device did
                c.rt().mem().w32(c.rt().lib_object("_ZN9GS3DStuff12s_perfConfigE"), cfg.quality == "low" ? 0 : 1);
            return false;
        },
        [cfg](cpu::Cpu& c) {
            if (cfg.quality != "phone") apply_quality(cfg, c);
            log_profile(c, cfg.quality.c_str());
        },
    });

    // ---- HUD ------------------------------------------------------------------------------------
    rt.hook_function("_ZN11CHudManager4loadEv", {[](cpu::Cpu& c) {
        g_hud = c.reg(cpu::R0);
        LOG("HUD: CHudManager at %08x", g_hud);
        return false;
    }, nullptr});
    for (const char* fn : {"_ZNK11AnalogStick6draw2dEv", "_ZNK14AnimatedButton6draw2dEv", "_ZNK10HudElement6draw2dEv",
                           "_ZNK12SlideControl6draw2dEv", "_ZNK11SwipeButton6draw2dEv", "_ZNK12ToggleButton6draw2dEv",
                           "_ZNK5Wheel6draw2dEv"})
        rt.hook_function(fn, {[](cpu::Cpu& c) { return g_hidden && is_control(c.reg(cpu::R0)); }, nullptr});

    // ---- statistics: the game's own time step (Application::_Update(int dt_ms)) --------------------
    rt.hook_function("_ZN11Application7_UpdateEi", {[](cpu::Cpu& c) {
        g_step_ms += int32_t(c.reg(cpu::R1));
        ++g_steps;
        return false;
    }, nullptr});

    // ---- camera modes ---------------------------------------------------------------------------
    // FollowCamera (on-foot camera): position at +0x08, look-at at +0x14 (Camera::getPosition /
    // getLookAt). FollowCamera::update moves the camera back behind the character every frame
    // (the phone's auto-centering); drags arrive as per-frame yaw/pitch deltas (+0x34/+0x38, from
    // updateAngles) that updatePosition applies as rotations around the target and then zeroes.
    // Freeform: keep the camera's offset from its look-at point across the game's update, so it
    // follows the character without swinging behind it; drags still rotate it.
    g_camera_mode = s.camera_mode;
    rt.hook_function("_ZN12FollowCamera6updateEf", {
        [](cpu::Cpu& c) {
            if (g_camera_mode != "freeform") return false;
            auto& m = c.rt().mem();
            const uint32_t cam = c.reg(cpu::R0);
            g_cam_self = cam;
            for (int i = 0; i < 3; ++i) {
                g_cam_look[i] = f32_at(cam + 0x14 + 4 * i);
                g_cam_off[i] = f32_at(cam + 0x08 + 4 * i) - g_cam_look[i];
            }
            (void)m;
            return false;
        },
        [](cpu::Cpu& c) {
            if (g_camera_mode != "freeform" || !g_cam_self) return;
            auto& m = c.rt().mem();
            const uint32_t cam = g_cam_self;
            g_cam_self = 0;
            float look[3], jump2 = 0, off2 = 0;
            for (int i = 0; i < 3; ++i) {
                look[i] = f32_at(cam + 0x14 + 4 * i);
                jump2 += (look[i] - g_cam_look[i]) * (look[i] - g_cam_look[i]);
                off2 += g_cam_off[i] * g_cam_off[i];
            }
            if (off2 <= 0 || jump2 > 4 * off2) return;  // cut, teleport or a new target: let the game place it
            for (int i = 0; i < 3; ++i) {
                const float v = look[i] + g_cam_off[i];
                uint32_t bits;
                std::memcpy(&bits, &v, 4);
                m.w32(cam + 0x08 + 4 * i, bits);
            }
        }});

    // First person (on foot): after CameraManager::update has placed the scene camera node
    // (this+0, vfuncs +0xA4 setPosition / +0x104 setTarget, the same calls the game makes), move the
    // node to the look-at point (the character's head) facing along the third-person view direction.
    // The game's camera logic keeps running unchanged, so drags/mouse still turn the view. Only while
    // the active camera (this+8) is the FollowCamera; driving, cutscenes etc. are untouched.
    rt.hook_function("_ZN13CameraManager6updateEi", {
        [](cpu::Cpu& c) {
            g_cm_self = g_camera_mode == "first_person" ? c.reg(cpu::R0) : 0;
            return false;
        },
        [](cpu::Cpu& c) {
            if (!g_cm_self) return;
            auto& r = c.rt();
            auto& m = r.mem();
            const uint32_t cm = g_cm_self;
            g_cm_self = 0;
            const uint32_t cam = m.r32(cm + 8), node = m.r32(cm);
            static const uint32_t follow_vt = r.lib_object("_ZTV12FollowCamera") + 8;
            if (!cam || !node || m.r32(cam) != follow_vt) return;
            float pos[3], look[3], d[3], len = 0;
            for (int i = 0; i < 3; ++i) {
                pos[i] = f32_at(cam + 0x08 + 4 * i);
                look[i] = f32_at(cam + 0x14 + 4 * i);
                d[i] = look[i] - pos[i];
                len += d[i] * d[i];
            }
            if (len <= 1e-6f) return;
            len = std::sqrt(len);
            static uint32_t scratch = 0;
            if (!scratch) scratch = r.heap().alloc(32);
            float eye[3], tgt[3];
            for (int i = 0; i < 3; ++i) {
                eye[i] = look[i];
                tgt[i] = look[i] + d[i] / len * 100.0f;
            }
            m.write(scratch, eye, 12);
            m.write(scratch + 16, tgt, 12);
            const uint32_t vt = m.r32(node);
            c.call(m.r32(vt + 0xa4), {node, scratch});        // setPosition
            c.call(m.r32(vt + 0x104), {node, scratch + 16});  // setTarget
        }});

    // ---- debugging: where does the follow camera's yaw/pitch get pulled back? ----------------------
    if (std::getenv("GS_WATCH_CAMERA")) {
        rt.hook_function("_ZN12FollowCamera12updateAnglesEii", {[](cpu::Cpu& c) {
            static int n = 0;
            if (n++ < 20)
                LOG("FollowCamera::updateAngles(this=%08x, dx=%d, dy=%d)", c.reg(cpu::R0), int32_t(c.reg(cpu::R1)),
                    int32_t(c.reg(cpu::R2)));
            if (!g_cam_watch_done && !g_cam_watch) g_cam_watch = c.reg(cpu::R0);
            return false;
        }, nullptr});
    }

    // ---- text changes (string tables served through hle/overrides) ---------------------------------
    // init.english: 0 "Loading", 1 "Touch the screen to continue", 2 copyright, 3 digits, 4 warning.
    // about.english (INFO > ABOUT): entries 0-5 are never shown; page 1 = entries 6 ("\0GANGSTAR...\1")
    // to 12, entry 13 "\n\n" breaks the page, page 2 starts the credits: 14 a blank line, 15
    // "\0PORT TEAM\1", ... ("\0" / "\1" are literal backslash codes for heading style, "\n" a blank
    // line). Replacing blank line 14 keeps the entry count, so nothing that indexes the table shifts.
    if (s.custom_text) {
        hle::overrides::add("init.english", [](std::vector<uint8_t>& d) {
            return hle::overrides::string_table_set(d, 1, "Press any button to start");
        });
        // Uppercase on purpose: like the other credit headings. Lowercase letters in heading style
        // are glyphs the game never pre-renders, and adding them overflowed its glyph cache at boot,
        // which made the title screen prompt disappear (tested).
        hle::overrides::add("about.english", [](std::vector<uint8_t>& d) {
            return hle::overrides::string_table_set(d, 14, "\\0DECOMP BY AIDENCANTCODE\\1");
        });
    }

    // ---- main menu: remove the dead Gameloft store buttons -----------------------------------------
    // Widgets::MainMenu is the Flash movie "widgetMainMenu"; its buttons btnGLLive (event 3 ->
    // Gameloft LIVE!) and btnMore (event 4 -> More Games / IGP) lead to services that no longer exist.
    // The game itself hides widgetMainMenu.btnTwitter the same way (RenderFX::SetVisible(path, false)).
    if (s.hide_store_buttons) {
        rt.hook_function("_ZN5Menus8MainMenu8GotFocusEv", {nullptr, [](cpu::Cpu& c) {
            auto& r = c.rt();
            static uint32_t paths[2] = {};
            if (!paths[0]) {
                const char* names[2] = {"widgetMainMenu.btnGLLive", "widgetMainMenu.btnMore"};
                for (int i = 0; i < 2; ++i) {
                    paths[i] = r.heap().alloc(uint32_t(std::strlen(names[i]) + 1));
                    r.mem().write(paths[i], names[i], uint32_t(std::strlen(names[i]) + 1));
                }
            }
            const uint32_t mgr = uint32_t(c.call(r.lib_func("_ZN11MenuManager11getInstanceEv")));
            const uint32_t fx = r.mem().r32(mgr + 0xb4);  // MenuManager's RenderFX (see MenuWidget ctor)
            for (uint32_t p : paths) c.call(r.lib_func("_ZN8RenderFX10SetVisibleEPKcb"), {fx, p, 0});
        }});
        rt.hook_function("_ZN5Menus8MainMenu6NotifyEPK10MenuWidgeti", {[](cpu::Cpu& c) {
            const uint32_t ev = c.reg(cpu::R2);
            if (ev != 3 && ev != 4) return false;
            c.set_reg(cpu::R0, 0);  // ignore a click that still reaches them
            return true;
        }, nullptr});
    }

    // ---- virtual steering (tilt mode) -------------------------------------------------------------
    rt.hook_function("_ZN21DrivingControlHandler18handleAccelerationEPK14EvAcceleration", {[](cpu::Cpu& c) {
        if (!g_steer_on) return false;  // real accelerometer: the game's own code
        auto& m = c.rt().mem();
        const uint32_t veh = m.r32(c.reg(cpu::R0) + 4);
        if (!veh) return true;
        const float s = std::clamp(g_steer, -1.0f, 1.0f);  // same convention: -1 = left (verified by driving)
        uint32_t bits;
        std::memcpy(&bits, &s, 4);
        m.w32(veh + 0x3e4, bits);
        // The original clears these two on every accelerometer event as well (purpose unknown).
        m.w32(veh + 0x3e8, 0);
        if (const uint32_t p = m.r32(veh + 0x280)) m.w32(p + 0x448, 0);
        return true;
    }, nullptr});
}

void set_camera_mode(const std::string& mode) { g_camera_mode = mode; }

uint32_t take_camera_to_watch() {
    const uint32_t a = g_cam_watch;
    if (a) {
        g_cam_watch = 0;
        g_cam_watch_done = true;
    }
    return a;
}

bool take_step_stats(double* avg_step_ms, int* steps) {
    if (!g_steps) return false;
    *avg_step_ms = double(g_step_ms) / double(g_steps);
    *steps = int(g_steps);
    g_step_ms = g_steps = 0;
    return true;
}

void set_virtual_steering(bool on, float steer) {
    g_steer_on = on;
    g_steer = steer;
}

void set_touch_controls_hidden(bool hidden) {
    if (hidden != g_hidden) LOG("HUD: touch controls %s", hidden ? "hidden" : "shown");
    g_hidden = hidden;
}

bool touch_controls_hidden() { return g_hidden; }

int steering_mode(rt::Runtime& rt) { return g_hud ? int32_t(rt.mem().r32(g_hud + 0x1c)) : -1; }

std::vector<Control> controls(rt::Runtime& rt, int screen_w, int screen_h) {
    std::vector<Control> out;
    if (!g_hud) return out;
    auto& m = rt.mem();
    const float sx = screen_w / 480.0f, sy = screen_h / 320.0f;  // Application::GetScreenScaleFactors
    for (const ControlField& f : kControls) {
        uint32_t obj = m.r32(g_hud + f.offset);
        if (!obj) continue;
        Control c{f.offset, f.kind, obj, -1, (m.r32(obj + 0x0c) & 1) != 0,
                  f32_at(obj + 0x10) * sx, f32_at(obj + 0x14) * sy, f32_at(obj + 0x18) * sx, f32_at(obj + 0x1c) * sy};
        if (!std::strcmp(f.kind, "VirtualButton") || !std::strcmp(f.kind, "AnimatedButton"))
            c.type = int32_t(m.r32(obj + 0x58));
        if (!std::strcmp(f.kind, "AnalogStick")) c.stick_radius = int32_t(m.r32(obj + 0x5c));
        out.push_back(c);
    }
    return out;
}

void log_controls(rt::Runtime& rt, int screen_w, int screen_h) {
    auto cs = controls(rt, screen_w, screen_h);
    LOG("HUD controls (%zu, screen %dx%d):", cs.size(), screen_w, screen_h);
    for (const Control& c : cs)
        LOG("  +%02x %-14s type %2d %s rect (%4.0f,%4.0f)-(%4.0f,%4.0f) radius %d", c.field, c.kind, c.type,
            c.visible ? "VISIBLE" : "hidden ", c.x0, c.y0, c.x1, c.y1, c.stick_radius);
}

}  // namespace mods
