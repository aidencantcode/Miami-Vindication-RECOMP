#pragma once
// Game-specific enhancements built on function hooks (rt::Runtime::hook_function).
// Everything here comes from reverse engineering libGangster2 (see NOTES.md, "Phase 3 RE"):
//
//   Graphics profile: GS3DStuff::loadPerformanceProfile reads entry GS3DStuff::s_perfConfig of
//   PhonePerformanceProfiles.gmap into gPhonePerf (44 bytes):
//     +0x00..+0x09  10 feature flags (+0x04 = mipmaps, read by UseMipMaps())
//     +0x0C  texture streaming radius      (low 7500  / high 12000)
//     +0x10  low-res city far plane        (low 20000 / high 60000; GetLowResFarPlane())
//     +0x14  ?                             (low 6     / high 16)
//     +0x18  ?                             (low 66    / high 50)
//     +0x1C, +0x20, +0x24  max NPCs per spawn-zone type (low 3,3,2 / high 10,10,10)
//     +0x28, +0x29  flags (+0x29 picks *_lo / *_hi asset variants; none exist in this data)
//   The phone used profile 0 (its GPU name matched no entry in PhonePerformanceSelector.array).
//
//   HUD controls: CHudManager (captured in CHudManager::load) owns the on-screen controls at
//   fixed fields; each HudElement keeps flags at +0x0C (bit0 visible) and its touch rectangle
//   (x0,y0,x1,y1) at +0x10 in 480x320 "authoring" space, scaled by screen/480 and screen/320.
#include <cstdint>
#include <string>
#include <vector>

namespace rt { class Runtime; }
namespace cpu { class Cpu; }
namespace settings { struct Settings; }

namespace mods {

void install(rt::Runtime& rt, const settings::Settings& s);

// Touch-control drawing (input keeps working either way).
void set_touch_controls_hidden(bool hidden);
bool touch_controls_hidden();

struct Control {
    int field;          // offset inside CHudManager
    const char* kind;   // class name
    uint32_t obj;       // guest pointer
    int type;           // VirtualButton ButtonType (-1 for non-buttons)
    bool visible;
    float x0, y0, x1, y1;  // rectangle in game screen pixels
    int stick_radius = 0;  // AnalogStick: full deflection in 480x320 units (+0x5C); 0 otherwise
};
// CHudManager::SteeringMode (CHudManager+0x1C); -1 before the HUD exists.
// 0x800000 = tilt (DrivingControlHandler::handleAcceleration steers from the accelerometer);
// the other modes (0x80000, 2, 1) show an on-screen Wheel / SlideControl / buttons. Which is
// which is still an open question (NOTES.md).
constexpr int kSteerTilt = 0x800000;
int steering_mode(rt::Runtime& rt);
// Virtual steering for tilt mode (pad / keyboard): -1 = full left .. +1 = full right.
// While on, DrivingControlHandler::handleAcceleration is replaced: it writes the vehicle's
// steering input (+0x3E4, where the game puts min(|tilt| * 1.8, 1)) directly, skipping the
// tilt maths and the camera roll that goes with it. Accelerometer events must still arrive
// (input feeds a neutral reading every frame) because they are what runs the handler.
void set_virtual_steering(bool on, float steer);

// Camera mode (classic | freeform | first_person), live.
void set_camera_mode(const std::string& mode);

// Aiming (PC scheme unless [controls] tap_to_target): no tap-to-target and no automatic lock;
// auto-aim held (LT / middle mouse) locks the game's own pick, release unlocks.
void set_aim_input(bool auto_aim);
// Queue weapon switches (+1 next, -1 previous); done in frame() through Player::switchWeapon.
void switch_weapon(int step);
// Once per frame before the game runs (main CPU idle): weapon switches, auto-aim.
void frame(cpu::Cpu& c, double dt);

// The game's own time step since the last call: average ms per Application::_Update and count.
bool take_step_stats(double* avg_step_ms, int* steps);

// Debugging (GS_WATCH_CAMERA=1): the FollowCamera seen by updateAngles; 0 = none yet / already taken.
uint32_t take_camera_to_watch();

// Current controls (empty until the HUD has been created). screen = the game's logical size.
std::vector<Control> controls(rt::Runtime& rt, int screen_w, int screen_h);
void log_controls(rt::Runtime& rt, int screen_w, int screen_h);

}  // namespace mods
