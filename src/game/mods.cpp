#include "game/mods.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>

#include "core/layout.h"
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
uint32_t g_cam_self = 0;                // Follow/VehicleCamera being updated (pre -> post)
float g_cam_dt = 0;                     // its update's time step (seconds)
uint32_t g_cm_self = 0;                 // CameraManager being updated (pre -> post), first person only
// Free look (Q2): our own yaw/pitch of the camera around the game's look-at point.
struct FreeLook {
    uint32_t cam = 0;
    float yaw = 0, pitch = 0;           // direction from the look-at point to the camera (radians, Z up)
    float look[3] = {};
    double idle = 0;                    // seconds without camera input
    bool valid = false;
};
FreeLook g_fl;
float g_cam_in_x = 0, g_cam_in_y = 0;   // camera input not applied yet (game pixels)
int g_frame_no = 0, g_fl_frame = -100;  // mods::frame count; the frame free look last ran
float g_fl_return_delay = 1.5f, g_fl_return_speed = 90.0f;  // seconds; degrees per second
constexpr float kFreeLookRadPerPixel = 0.0045f;  // ~0.26 degrees per game pixel of mouse / stick
bool free_look_mode() { return g_camera_mode == "freelook" || g_camera_mode == "first_person"; }

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

// ---- aiming (Phase 5A.4, NOTES §5A.4) ----------------------------------------------------------
// The phone aims two ways: a short tap on empty screen locks onto whatever is under it
// (HudScreen::processTouchRelease -> tappedTarget -> Player::lockTarget(t, true)), and while the
// weapon is out Player::updateLockTarget locks automatically onto Character::getAttackableTarget()
// (the nearest non-friendly character it can attack, preferring ones in front). On PC both fired
// at random: camera drags that barely move are released as taps at the drag anchor, and shooting
// grabbed the nearest target. PC scheme: no taps, no automatic lock; holding auto-aim (LT / middle
// mouse) asks the game for its target and locks it like a tap does, releasing unlocks.
// Q1 (owner): firing without the crosshair auto-aims like the phone (the game's own pick, while fire
// is held and briefly after, so semi-automatic taps don't swing the player back and forth); a middle
// click locks the nearest target until clicked again.
bool g_pc_aim = true;            // false: the phone's behaviour (touchscreens)
bool g_auto_aim = false, g_auto_aim_was = false;  // lock held (LT)
bool g_lock_toggled = false, g_toggle_request = false;  // middle click lock
bool g_firing = false, g_fire_suppressed = false;
double g_fire_hold = 0;           // seconds the fire auto-aim stays on after the last shot
constexpr double kFireAimHold = 1.0;
bool g_fire_auto = false;         // the game may pick the player's target (fire auto-aim active)
bool g_reload_request = false;
float g_manual_spread = 0.25f;    // manual aim: fraction of the game's random bullet spread
uint32_t g_spread_char = 0;       // Character whose spread factors are scaled during Weapon::shoot
uint32_t g_spread_saved[2] = {};
bool g_allow_auto_target = false;  // our own getAttackableTarget call
uint32_t g_gat_self = 0;           // getAttackableTarget's this (pre -> post)
double g_reacquire = 0;            // seconds until the next auto-aim search
int g_weapon_steps = 0;            // queued weapon switches (+next / -previous)
int g_screen_w = 800, g_screen_h = 480;  // the game's logical screen (from controls())
// Manual aim (RT / right mouse): Character::aim(bool) already aims the player at the screen
// centre when no target is locked and the weapon is out (Player+0x688 set; the phone did this
// after shooting with nothing locked). Holding aim keeps the weapon out and moves the camera
// over the shoulder; the crosshair marks the screen centre.
bool g_manual_aim = false, g_manual_was = false;
float g_aim_blend = 0;                   // 0 = game camera, 1 = over the shoulder
float g_aim_eye[3] = {}, g_aim_dir[3] = {};  // the aim camera (eye, unit view direction) last placed
bool g_aim_valid = false;
constexpr uint32_t kPlayerWeaponOut = 0x688;  // Player: weapon drawn / aiming (time, 0 = lowered)
// GANGSTAR_INPUT_TRACE=1: per-frame log of the player and camera positions (input latency checks).
const bool g_input_trace = std::getenv("GANGSTAR_INPUT_TRACE") != nullptr;
int g_trace_frame = 0;
uint32_t g_follow_cam = 0;  // the on-foot FollowCamera (last FollowCamera::update)
uint32_t g_trace_cam = 0;   // the Follow/VehicleCamera updated last (input trace)
// Physics step (see the Application::_Update hook): the literal-pool word in GS3DStuff::update.
bool g_smooth_physics = true;
std::vector<std::string> g_debug_cmds;  // scripted test commands (input "mod <name>"), run in frame()
constexpr uint32_t kPhysicsStepLiteral = layout::kLibBase + 0x289dc0;
constexpr uint32_t kPhysicsStep60 = 0x3c888889;  // 1/60 f, the game's value

uint32_t player_obj(rt::Runtime& r);
void shoulder_camera(cpu::Cpu& c, uint32_t cam, uint32_t node);

uint32_t player_obj(rt::Runtime& r) {
    static const uint32_t var = r.lib_object("_ZN6Player8s_playerE");
    return r.mem().r32(var);
}
constexpr uint32_t kPlayerTarget = 0x650;  // Player: locked target (Player::_GetTarget)

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

void free_look_update(cpu::Cpu& c, uint32_t cam) {
    auto& m = c.rt().mem();
    float look[3], off[3], dist = 0;
    for (int i = 0; i < 3; ++i) {
        look[i] = f32_at(cam + 0x14 + 4 * i);
        off[i] = f32_at(cam + 0x08 + 4 * i) - look[i];
        dist += off[i] * off[i];
    }
    dist = std::sqrt(dist);
    if (!(dist > 1.0f) || !std::isfinite(dist)) return;
    const float game_yaw = std::atan2(off[1], off[0]);
    const float game_pitch = std::asin(std::clamp(off[2] / dist, -1.0f, 1.0f));
    float jump = 0;
    for (int i = 0; i < 3; ++i) jump += (look[i] - g_fl.look[i]) * (look[i] - g_fl.look[i]);
    jump = std::sqrt(jump);
    const float dt = std::clamp(g_cam_dt, 0.0005f, 0.1f);
    // A new camera (got in / out of a car), a cut or a teleport: start from the game's view.
    if (!g_fl.valid || g_fl.cam != cam || jump > 2.0f * dist || g_frame_no - g_fl_frame > 2) {
        g_fl.yaw = game_yaw;
        g_fl.pitch = game_pitch;
        g_fl.cam = cam;
        g_fl.idle = 0;
        g_fl.valid = true;
        jump = 0;
    }
    g_fl_frame = g_frame_no;
    std::memcpy(g_fl.look, look, sizeof look);
    if (g_cam_in_x != 0 || g_cam_in_y != 0) {  // right = turn right, down = look down
        g_fl.yaw -= g_cam_in_x * kFreeLookRadPerPixel;
        g_fl.pitch += g_cam_in_y * kFreeLookRadPerPixel;
        g_fl.idle = 0;
    } else {
        g_fl.idle += dt;
    }
    g_cam_in_x = g_cam_in_y = 0;
    const float speed = jump / dt;  // how fast the look-at point (player / car) moves
    if (g_camera_mode != "first_person" && g_fl.idle > g_fl_return_delay && speed > 60.0f) {
        const float step = g_fl_return_speed * 0.017453f * dt;
        float d = game_yaw - g_fl.yaw;
        d = std::remainder(d, 6.2831853f);
        g_fl.yaw += std::clamp(d, -step, step);
        g_fl.pitch += std::clamp(game_pitch - g_fl.pitch, -step * 0.5f, step * 0.5f);
    }
    g_fl.yaw = std::remainder(g_fl.yaw, 6.2831853f);
    g_fl.pitch = std::clamp(g_fl.pitch, -0.35f, 1.2f);  // from a little below the head to steeply down
    const float cp = std::cos(g_fl.pitch);
    const float pos[3] = {look[0] + dist * cp * std::cos(g_fl.yaw), look[1] + dist * cp * std::sin(g_fl.yaw),
                          look[2] + dist * std::sin(g_fl.pitch)};
    m.write(cam + 0x08, pos, 12);
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

uint32_t target_at_screen_center(cpu::Cpu& c);  // below

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
    // Also smooth physics: GS3DStuff::update steps Box2D and every object's pre/post-physics with a
    // fixed 1/60 s step from an accumulator, and nothing interpolates between steps, so above 60 fps
    // people, cars and the camera following them only moved on some frames (every other frame at
    // 120). The step is a literal-pool word that the recompiler reads from memory
    // (codegen.py RUNTIME_LITERALS); setting it to this frame's dt gives one step per frame. Slow
    // frames (dt > 1/60) keep the game's 1/60 steps.
    g_smooth_physics = s.smooth_physics;
    rt.hook_function("_ZN11Application7_UpdateEi", {[](cpu::Cpu& c) {
        const int32_t dt = int32_t(c.reg(cpu::R1));
        g_step_ms += dt;
        ++g_steps;
        uint32_t bits = kPhysicsStep60;
        if (g_smooth_physics && dt < 17) {
            const float step = float(std::max(dt, 1)) / 1000.0f;  // the same float the game adds to its accumulator
            std::memcpy(&bits, &step, 4);
        }
        auto& m = c.rt().mem();
        static int writable = 0;  // 0 untried, 1 yes, -1 no
        if (!writable) {  // here, not in install(): the recompiled backend protects code pages later
            writable = m.host_writable(kPhysicsStepLiteral, 4) ? 1 : -1;
            LOG("physics: step word %s (smooth_physics %s)", writable > 0 ? "patchable" : "NOT writable",
                g_smooth_physics ? "on" : "off");
        }
        if (writable < 0) return false;
        if (m.r32(kPhysicsStepLiteral) != bits) m.w32(kPhysicsStepLiteral, bits);
        return false;
    }, nullptr});

    // ---- camera modes ---------------------------------------------------------------------------
    // FollowCamera (on foot) and VehicleCamera (cars) are Camera subclasses: position at +0x08,
    // look-at at +0x14 (Camera::getPosition / getLookAt), update(float seconds) via CameraManager::
    // update. FollowCamera::update moves the camera back behind the character every frame (the
    // phone's auto-centering); drags arrive as per-frame yaw/pitch deltas (+0x34/+0x38, from
    // updateAngles) that updatePosition applies as rotations around the target and then zeroes.
    // Free look (Q2, GTA Vice City PC style; "freeform" in old settings files means the same): after
    // the game's update, the camera is put at our own yaw/pitch around the game's look-at point, at
    // the game's distance. Mouse / right stick turn it directly (camera_input, no drags), it stays
    // where it was left, and after freelook_return_delay seconds without camera input, while moving,
    // it turns back toward where the game would put it (behind) at freelook_return_speed. Writing
    // the game camera (not only the scene node) keeps "forward" for the stick, aiming and first
    // person consistent with the view.
    g_camera_mode = s.camera_mode == "freeform" ? "freelook" : s.camera_mode;
    g_fl_return_delay = std::max(0.0f, s.freelook_return_delay);
    g_fl_return_speed = std::max(0.0f, s.freelook_return_speed);
    for (const char* fn : {"_ZN12FollowCamera6updateEf", "_ZN13VehicleCamera6updateEf"}) {
        const bool vehicle = std::strstr(fn, "Vehicle") != nullptr;
        rt.hook_function(fn, {
            [vehicle](cpu::Cpu& c) {
                if (!vehicle) g_follow_cam = c.reg(cpu::R0);
                g_cam_self = g_trace_cam = c.reg(cpu::R0);
                const uint32_t bits = c.reg(cpu::R1);
                std::memcpy(&g_cam_dt, &bits, 4);
                return false;
            },
            [](cpu::Cpu& c) {
                const uint32_t cam = g_cam_self;
                g_cam_self = 0;
                if (cam && free_look_mode()) free_look_update(c, cam);
            }});
    }

    // First person (on foot): after CameraManager::update has placed the scene camera node
    // (this+0, vfuncs +0xA4 setPosition / +0x104 setTarget, the same calls the game makes), move the
    // node to the look-at point (the character's head) facing along the third-person view direction.
    // The game's camera logic keeps running unchanged, so drags/mouse still turn the view. Only while
    // the active camera (this+8) is the FollowCamera; driving, cutscenes etc. are untouched.
    rt.hook_function("_ZN13CameraManager6updateEi", {
        [](cpu::Cpu& c) {
            g_cm_self = g_camera_mode == "first_person" || g_aim_blend > 0 ? c.reg(cpu::R0) : 0;
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
            if (g_aim_blend > 0) {
                shoulder_camera(c, cam, node);
                return;
            }
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

    // ---- aiming (see the top of this file) --------------------------------------------------------
    g_pc_aim = !s.tap_to_target;
    // A release always counts as a drag (moved 100 px), so it never becomes a tap-to-target.
    rt.hook_function("_ZN9HudScreen19processTouchReleaseEv", {[](cpu::Cpu& c) {
        if (g_pc_aim) c.rt().mem().w32(c.reg(cpu::R0) + 0x6c, 100);
        return false;
    }, nullptr});
    // The player's automatic target: none unless auto-aim is asking.
    rt.hook_function("_ZN9Character19getAttackableTargetEv", {
        [](cpu::Cpu& c) {
            g_gat_self = c.reg(cpu::R0);
            return false;
        },
        [](cpu::Cpu& c) {
            const uint32_t self = g_gat_self;
            g_gat_self = 0;
            if (!g_pc_aim || g_allow_auto_target || g_fire_auto) return;
            const uint32_t p = player_obj(c.rt());
            if (p && self == p + 4) c.set_reg(cpu::R0, 0);
        }});

    // Manual aim: every bullet of the player goes to the crosshair. Weapon::shoot(Character*,
    // vector3d from, vector3d dir, bool): vector3d has a copy constructor, so GCC passes both by
    // invisible reference (r2 = &from, r3 = &dir, caller-owned temporaries; checked: reading them
    // as floats gave NaNs). The game points the gun at the far end of the crosshair ray, so close
    // targets were missed by the camera-to-gun offset; aim from the muzzle at what is under the
    // crosshair instead (a person's chest, a vehicle), else at the crosshair ray's far point.
    // Spread: per axis rand(-100..100) * 0.01 * weapon (+0x4C) * shooter (Character +0x6C8, or +0x6CC
    // when its vfunc +0x110 says so), added to the unit direction. Manual-aim shots scale both shooter
    // factors by manual_aim_spread for the call (restored in the post hook).
    g_manual_spread = std::clamp(s.manual_aim_spread, 0.0f, 1.0f);
    rt.hook_function("_ZN6Weapon5shootEP9CharacterN6glitch4core8vector3dIfEES5_b", {[](cpu::Cpu& c) {
        if (g_input_trace) LOG("[trace] f%d Weapon::shoot", g_trace_frame);
        if (!g_pc_aim || g_aim_blend < 0.5f || !g_aim_valid) return false;
        auto& r = c.rt();
        auto& m = r.mem();
        const uint32_t p = player_obj(r);
        if (!p || c.reg(cpu::R1) != p + 4) return false;
        if (!g_spread_char) {
            g_spread_char = p + 4;
            for (int i = 0; i < 2; ++i) {
                const uint32_t a = g_spread_char + 0x6c8 + 4 * i;
                g_spread_saved[i] = m.r32(a);
                const float f = f32_at(a) * g_manual_spread;
                uint32_t bits;
                std::memcpy(&bits, &f, 4);
                m.w32(a, bits);
            }
            static int logged_spread = 0;
            if (logged_spread++ < 3)
                LOG("aim: spread weapon %.3f, shooter %.3f / %.3f, scaled by %.2f", f32_at(c.reg(cpu::R0) + 0x4c),
                    f32_at(g_spread_char + 0x6c8) / std::max(g_manual_spread, 1e-6f),
                    f32_at(g_spread_char + 0x6cc) / std::max(g_manual_spread, 1e-6f), g_manual_spread);
        }
        const uint32_t from_p = c.reg(cpu::R2), dir_p = c.reg(cpu::R3);
        if (!from_p || !dir_p) return false;
        float from[3], dir[3];
        for (int i = 0; i < 3; ++i) {
            from[i] = f32_at(from_p + 4 * i);
            dir[i] = f32_at(dir_p + 4 * i);
            if (!std::isfinite(from[i]) || !std::isfinite(dir[i])) return false;
        }
        float aim[3];
        const uint32_t t = target_at_screen_center(c);
        static uint32_t scratch = 0;
        if (!scratch) scratch = r.heap().alloc(16);
        if (t) {
            c.call(m.r32(m.r32(t) + 0x5c), {scratch, t});  // LevelObject::getPosition (feet)
            const bool person = c.call(r.lib_func("_ZN17GameObjectManager11isCharacterEPK11LevelObject"), {t}) != 0;
            for (int i = 0; i < 3; ++i) aim[i] = f32_at(scratch + 4 * i);
            aim[2] += person ? 110.0f : 60.0f;  // chest / body
        } else {
            // Nothing under the crosshair: where its ray meets the ground at the player's feet
            // (no world raycast yet), else 60 m along it.
            float reach = 6000.0f;
            c.call(m.r32(m.r32(p + 4) + 0x5c), {scratch, p + 4});
            const float ground = f32_at(scratch + 8);
            if (g_aim_dir[2] < -0.01f) reach = std::min(reach, (g_aim_eye[2] - ground) / -g_aim_dir[2]);
            for (int i = 0; i < 3; ++i) aim[i] = g_aim_eye[i] + g_aim_dir[i] * reach;
        }
        float nd[3], l = 0;
        for (int i = 0; i < 3; ++i) l += (nd[i] = aim[i] - from[i]) * nd[i];
        if (l < 1) return false;
        l = std::sqrt(l);
        for (int i = 0; i < 3; ++i) {
            nd[i] /= l;
            uint32_t bits;
            std::memcpy(&bits, &nd[i], 4);
            m.w32(dir_p + 4 * i, bits);
        }
        static int logged = 0;
        if (logged++ < 8)
            LOG("aim: shot from %.0f %.0f %.0f dir %.2f %.2f %.2f (|%.2f|) -> %.2f %.2f %.2f at %s %08x (%.0f away)", from[0],
                from[1], from[2], dir[0], dir[1], dir[2], std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]),
                nd[0], nd[1], nd[2], t ? "target" : "far point", t, l);
        return false;
    }, [](cpu::Cpu& c) {
        if (!g_spread_char) return;
        for (int i = 0; i < 2; ++i) c.rt().mem().w32(g_spread_char + 0x6c8 + 4 * i, g_spread_saved[i]);
        g_spread_char = 0;
    }});

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

void set_camera_mode(const std::string& mode) {
    g_camera_mode = mode == "freeform" ? "freelook" : mode;
    g_fl.valid = false;
}
bool free_look_on() { return free_look_mode() && g_frame_no - g_fl_frame <= 2; }
void camera_input(float dx, float dy) {
    g_cam_in_x += dx;
    g_cam_in_y += dy;
}
void set_free_look_return(float delay_s, float speed_deg) {
    g_fl_return_delay = std::max(0.0f, delay_s);
    g_fl_return_speed = std::max(0.0f, speed_deg);
}
void set_smooth_physics(bool on) { g_smooth_physics = on; }
void debug_command(const std::string& cmd) { g_debug_cmds.push_back(cmd); }

// The person or vehicle at the screen centre: the phone's tap picker (HudScreen::tappedTarget: a ray
// from the camera through the point, hit boxes that grow with distance; people first, then
// vehicles; never the dead). Its `this` is not used, so any pointer will do.
namespace {
// Over-the-shoulder aim camera (GTA V style): from the follow camera's position and look-at point
// (the character's upper body), keep its view direction but move the eye close behind the right
// shoulder; blended in and out by g_aim_blend. Set on the scene camera node like first person.
void shoulder_camera(cpu::Cpu& c, uint32_t cam, uint32_t node) {
    auto& r = c.rt();
    auto& m = r.mem();
    float pos[3], look[3], d[3], len = 0;
    for (int i = 0; i < 3; ++i) {
        pos[i] = f32_at(cam + 0x08 + 4 * i);
        look[i] = f32_at(cam + 0x14 + 4 * i);
        d[i] = look[i] - pos[i];
        len += d[i] * d[i];
    }
    if (len <= 1e-6f) return;
    len = std::sqrt(len);
    for (float& v : d) v /= len;
    // Z is up (logged: player z 185 at the feet, look-at = head at +180, camera 455 units back);
    // right = forward x up.
    const float up[3] = {0, 0, 1};
    float right[3] = {d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0]};
    const float rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (rl < 1e-4f) return;
    for (float& v : right) v /= rl;
    const float back = len * 0.62f, side = len * 0.22f, rise = len * 0.02f;  // relative to the game's distance
    const float t = g_aim_blend * g_aim_blend * (3 - 2 * g_aim_blend);  // smoothstep
    // The follow camera looks ~9 degrees down at the player; aim ~4 degrees higher, so the crosshair
    // rests near chest height instead of on the pavement (the player can still look up and down).
    float ad[3] = {d[0], d[1], d[2] + 0.07f * t}, al = 0;
    for (float v : ad) al += v * v;
    al = std::sqrt(al);
    for (float& v : ad) v /= al;
    float eye[3], tgt[3];
    for (int i = 0; i < 3; ++i) {
        const float ots = look[i] - d[i] * back + right[i] * side + up[i] * rise;
        eye[i] = pos[i] + (ots - pos[i]) * t;
        tgt[i] = eye[i] + ad[i] * 1000.0f;
    }
    static uint32_t buf = 0;
    if (!buf) buf = r.heap().alloc(32);
    m.write(buf, eye, 12);
    m.write(buf + 16, tgt, 12);
    for (int i = 0; i < 3; ++i) {  // what manual-aim shots aim along (see the Weapon::shoot hook)
        g_aim_eye[i] = eye[i];
        g_aim_dir[i] = ad[i];
    }
    g_aim_valid = true;
    const uint32_t vt = m.r32(node);
    c.call(m.r32(vt + 0xa4), {node, buf});        // setPosition
    c.call(m.r32(vt + 0x104), {node, buf + 16});  // setTarget
}

}  // namespace

uint32_t target_at_screen_center(cpu::Cpu& c) {
    auto& r = c.rt();
    static uint32_t scratch = 0;
    if (!scratch) scratch = r.heap().alloc(32);
    const int32_t pos[2] = {g_screen_w / 2, g_screen_h / 2};
    const float zero[3] = {0, 0, 0};
    r.mem().write(scratch, pos, 8);
    r.mem().write(scratch + 16, zero, 12);
    return uint32_t(c.call(r.lib_func("_ZNK9HudScreen12tappedTargetERKN6glitch4core10position2dIiEERKNS1_8vector3dIfEE"),
                           {g_hud ? g_hud : scratch, scratch, scratch + 16}));
}

void set_aim_input(bool lock_held, bool manual_aim, bool firing) {
    g_auto_aim = lock_held;
    g_manual_aim = manual_aim && !lock_held;
    g_firing = firing;
}
void toggle_lock() { g_toggle_request = true; }
void reload() { g_reload_request = true; }
void set_manual_aim_spread(float f) { g_manual_spread = std::clamp(f, 0.0f, 1.0f); }
bool manual_aiming() { return g_aim_blend > 0.5f; }
void switch_weapon(int step) { g_weapon_steps = std::clamp(g_weapon_steps + step, -3, 3); }

void frame(cpu::Cpu& c, double dt) {
    auto& r = c.rt();
    auto& m = r.mem();
    ++g_frame_no;
    const uint32_t p = player_obj(r);
    if (!p) {
        g_weapon_steps = 0;
        return;
    }
    if (g_input_trace) {
        static uint32_t scratch = 0;
        if (!scratch) scratch = r.heap().alloc(16);
        c.call(m.r32(m.r32(p + 4) + 0x5c), {scratch, p + 4});  // LevelObject::getPosition
        float cp[3] = {}, cl[3] = {};
        if (const uint32_t fc = g_trace_cam)
            for (int i = 0; i < 3; ++i) cp[i] = f32_at(fc + 0x08 + 4 * i), cl[i] = f32_at(fc + 0x14 + 4 * i);
        LOG("[trace] f%d dt %.1f ms: player %.1f %.1f cam %.1f %.1f look %.1f %.1f yaw %.1f", ++g_trace_frame, dt * 1000,
            f32_at(scratch), f32_at(scratch + 4), cp[0], cp[1], cl[0], cl[1],
            std::atan2(cl[1] - cp[1], cl[0] - cp[0]) * 57.29578f);
    }
    // Scripted test commands. nearcar: put the player 2.5 m beside the nearest empty car (the game's
    // Character::getClosestVehicleFrom, which also wants a clear line of sight), so a test can hold Y.
    for (const std::string& cmd : g_debug_cmds) {
        static uint32_t scratch = 0;
        if (!scratch) scratch = r.heap().alloc(16);
        if (cmd == "nearcar" || cmd == "nearheli") {
            uint32_t v = 0;
            if (cmd == "nearcar") {
                v = uint32_t(c.call(r.lib_func("_ZNK9Character21getClosestVehicleFromEb"), {p + 4, 1}));
                if (!v) v = uint32_t(c.call(r.lib_func("_ZNK9Character21getClosestVehicleFromEb"), {p + 4, 0}));
            } else {
                // Every level object: GameObjectManager::GetFirst(0), next at +0x0C.
                c.call(m.r32(m.r32(p + 4) + 0x5c), {scratch, p + 4});
                const float px = f32_at(scratch), py = f32_at(scratch + 4);
                const uint32_t gom = m.r32(r.lib_object("_ZN17GameObjectManager5s_gomE"));
                float best = 1e30f;
                int n = 0;
                for (uint32_t o = uint32_t(c.call(r.lib_func("_ZN17GameObjectManager8GetFirstEi"), {gom, 0})); o;
                     o = m.r32(o + 0x0c)) {
                    if (!c.call(r.lib_func("_ZN17GameObjectManager12isHelicopterEP11LevelObject"), {o})) continue;
                    c.call(m.r32(m.r32(o) + 0x5c), {scratch, o});
                    const float dx = f32_at(scratch) - px, dy = f32_at(scratch + 4) - py, d = dx * dx + dy * dy;
                    LOG("test: helicopter %08x at %.0f %.0f %.0f (%.0f away)", o, f32_at(scratch), f32_at(scratch + 4),
                        f32_at(scratch + 8), std::sqrt(d));
                    ++n;
                    if (d < best) best = d, v = o;
                }
                LOG("test: %d helicopters", n);
            }
            if (v) {
                c.call(m.r32(m.r32(v) + 0x5c), {scratch, v});  // LevelObject::getPosition
                float pos[3];
                for (int i = 0; i < 3; ++i) pos[i] = f32_at(scratch + 4 * i);
                pos[1] += 250.0f;
                m.write(scratch, pos, 12);
                c.call(r.lib_func("_ZN9Character11SetPositionERKN6glitch4core8vector3dIfEE"), {p + 4, scratch});
                LOG("test: player moved beside vehicle %08x at %.0f %.0f %.0f", v, pos[0], pos[1] - 250.0f, pos[2]);
                // What the enter button does (OnFootControlHandler::handleEnterCarButton): Player+0x704 is
                // the car the player may enter, normally set by a physics contact a teleport doesn't make.
                m.w32(p + 0x704, v);
                c.call(r.lib_func("_ZN6Player11initDrivingEb"), {p, 1});
            } else {
                LOG("test: nearcar: no vehicle found");
            }
        } else {
            LOG("test: unknown command '%s'", cmd.c_str());
        }
    }
    g_debug_cmds.clear();
    // Weapon switching: what the weapon selector's < > buttons call (handleSwitchWeaponButton).
    while (g_weapon_steps != 0) {
        const int step = g_weapon_steps > 0 ? 1 : -1;
        g_weapon_steps -= step;
        c.call(r.lib_func("_ZN6Player12switchWeaponEib"), {p, uint32_t(step), 1});
        LOG("weapon: switch %+d", step);
    }
    const bool in_vehicle = c.call(r.lib_func("_ZNK9Character12isInAVehicleEv"), {p + 4}) != 0;
    if (g_reload_request && !in_vehicle) {
        c.call(r.lib_func("_ZN9Character12reloadWeaponEv"), {p + 4});
        LOG("weapon: reload");
    }
    g_reload_request = false;
    if (!g_pc_aim) return;
    auto find_target = [&](const char** how) {
        g_allow_auto_target = true;
        uint32_t t = uint32_t(c.call(r.lib_func("_ZN9Character19getAttackableTargetEv"), {p + 4}));
        g_allow_auto_target = false;
        *how = "nearest attackable";
        if (!t) t = target_at_screen_center(c), *how = "at the screen centre";
        return t;
    };
    // Middle click: unlock if anything is locked, else lock the nearest target until clicked again
    // (or until it is gone). Unlocking while fire is held also pauses the fire auto-aim until release.
    uint32_t target = m.r32(p + kPlayerTarget);
    static uint32_t last_target = 0;
    if (target != last_target) {
        LOG("aim: target %08x -> %08x", last_target, target);
        last_target = target;
    }
    if (g_toggle_request && !in_vehicle) {
        if (target) {
            c.call(r.lib_func("_ZN6Player12delockTargetEv"), {p});
            g_lock_toggled = false;
            g_fire_suppressed = g_firing;
            g_fire_hold = 0;
            LOG("aim: lock off (middle click)");
        } else {
            const char* how;
            if (const uint32_t t = find_target(&how)) {
                c.call(r.lib_func("_ZN6Player10lockTargetEP11LevelObjectb"), {p, t, 1});
                g_lock_toggled = true;
                LOG("aim: lock on %08x (middle click, %s)", t, how);
            } else {
                LOG("aim: lock: nothing to lock onto");
            }
        }
        target = m.r32(p + kPlayerTarget);
    }
    g_toggle_request = false;
    if (g_lock_toggled && !target) {
        g_lock_toggled = false;
        LOG("aim: lock off (target gone)");
    }
    // Fire auto-aim: while fire is held (and a moment after), the game picks the target itself
    // (Player::updateLockTarget -> lockTarget(t, false)), exactly as on the phone; not while aiming
    // with the crosshair or while a lock is held/toggled.
    if (!g_firing) g_fire_suppressed = false;
    const bool fire_now = g_firing && !g_fire_suppressed && !g_manual_aim;
    g_fire_hold = fire_now ? kFireAimHold : std::max(0.0, g_fire_hold - dt);
    const bool fire_auto = g_fire_hold > 0 && !g_manual_aim && !g_auto_aim && !g_lock_toggled;
    if (fire_auto != g_fire_auto) LOG("aim: fire auto-aim %s", fire_auto ? "on" : "off");
    g_fire_auto = fire_auto;
    // Auto-aim: lock the game's own pick (as a tap would) while held; retry while nothing is locked.
    if (g_auto_aim) {
        g_reacquire -= dt;
        if (!target && g_reacquire <= 0) {
            const char* how;
            if (const uint32_t t = find_target(&how)) {
                c.call(r.lib_func("_ZN6Player10lockTargetEP11LevelObjectb"), {p, t, 1});
                LOG("aim: auto-aim locked %08x (%s; now %08x)", t, how, m.r32(p + kPlayerTarget));
            }
            g_reacquire = 0.25;
        }
    } else if (g_auto_aim_was && target && !g_lock_toggled) {
        c.call(r.lib_func("_ZN6Player12delockTargetEv"), {p});
        LOG("aim: auto-aim released");
    }
    if (!g_auto_aim) g_reacquire = 0;
    g_auto_aim_was = g_auto_aim;

    // Manual aim: keep the weapon out (the game then turns the player and points the gun at the
    // screen centre every update); lower it again on release if nothing else raised it. Aiming with
    // the crosshair drops any lock, so the gun follows the crosshair.
    const bool aiming = g_manual_aim && !in_vehicle;
    if (aiming) {
        if (!g_manual_was) {
            LOG("aim: manual aim on");
            if (m.r32(p + kPlayerTarget)) c.call(r.lib_func("_ZN6Player12delockTargetEv"), {p});
            g_lock_toggled = false;
        }
        if (m.r32(p + kPlayerWeaponOut) == 0) {
            c.call(r.lib_func("_ZN9Character10drawWeaponEv"), {p + 4});
            m.w32(p + kPlayerWeaponOut, 1);
        }
    } else if (g_manual_was) {
        if (m.r32(p + kPlayerWeaponOut) == 1) m.w32(p + kPlayerWeaponOut, 0);
        LOG("aim: manual aim off");
    }
    g_manual_was = aiming;
    const float rate = float(dt) / 0.15f;  // camera move in/out time
    g_aim_blend = std::clamp(g_aim_blend + (aiming ? rate : -rate), 0.0f, 1.0f);
    if (g_aim_blend == 0) g_aim_valid = false;
}

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
    g_screen_w = screen_w;
    g_screen_h = screen_h;
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
