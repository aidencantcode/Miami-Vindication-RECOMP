#include "game/mods.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

#include "core/log.h"
#include "rt/runtime.h"
#include "settings/settings.h"

namespace mods {

namespace {

rt::Runtime* g_rt = nullptr;
uint32_t g_hud = 0;  // CHudManager*
bool g_hidden = false;
bool g_steer_on = false;
float g_steer = 0;

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
        out.push_back(c);
    }
    return out;
}

void log_controls(rt::Runtime& rt, int screen_w, int screen_h) {
    auto cs = controls(rt, screen_w, screen_h);
    LOG("HUD controls (%zu, screen %dx%d):", cs.size(), screen_w, screen_h);
    for (const Control& c : cs)
        LOG("  +%02x %-14s type %2d %s rect (%4.0f,%4.0f)-(%4.0f,%4.0f)", c.field, c.kind, c.type,
            c.visible ? "VISIBLE" : "hidden ", c.x0, c.y0, c.x1, c.y1);
}

}  // namespace mods
