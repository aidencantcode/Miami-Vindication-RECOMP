#include "input/input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sstream>

#include "core/log.h"
#include "game/mods.h"
#include "rt/runtime.h"
#include "settings/settings.h"

namespace input {

namespace {
const char* kTouch = "Java_com_gameloft_android_GAND_GloftGMHP_ML_GameGLSurfaceView_nativeOnTouch";
const char* kKeyDown = "Java_com_gameloft_android_GAND_GloftGMHP_ML_Gangster2_nativeKeyDown";
const char* kKeyUp = "Java_com_gameloft_android_GAND_GloftGMHP_ML_Gangster2_nativeKeyUp";

enum { AK_BACK = 4, AK_MENU = 82 };  // Android KeyEvent codes

// One-shot buttons are a short virtual tap. Time-based, not frame-based: 3 frames were 150 ms at the
// emulator's 20 fps but 6 ms at 500 fps, too short for the game to register (weapon switching).
constexpr double kTapSeconds = 0.08;
constexpr int kTapMinFrames = 2;
constexpr double kTapGapSeconds = 0.06;  // released time between two queued taps (wheel notches)

// Pointer ids handed to the game (Android pointer ids are small integers).
enum { P_MOUSE = 0, P_STICK = 1, P_CAMERA = 2, P_FIRST_BUTTON = 3, P_LAST = 9 };

// ButtonType values (decoded from OnFoot/DrivingControlHandler::handleVirtualButton jump tables).
enum { BT_ATTACK = 0, BT_ENTER_CAR = 2, BT_SHOP = 3, BT_ACCEL = 4, BT_BRAKE = 5, BT_WEAPON_NEXT = 7,
       BT_WEAPON_PREV = 8, BT_MAP = 9, BT_RADIO_TOGGLE = 10, BT_RADIO = 11, BT_SIDE_MISSION = 13,
       BT_COVER = 14, BT_SPRINT = 15, BT_MINISAVE = 16 };

// Buttons each action may press, in order of preference (empty = not a HUD button).
const std::initializer_list<int> kActionTypes[A_COUNT] = {
    /*A_ATTACK*/ {BT_ATTACK}, /*A_ENTER_CAR*/ {BT_ENTER_CAR},
    /*A_INTERACT*/ {BT_SHOP, BT_SIDE_MISSION, BT_MINISAVE}, /*A_ACCELERATE*/ {BT_ACCEL},
    /*A_BRAKE*/ {BT_BRAKE}, /*A_COVER*/ {BT_COVER}, /*A_SPRINT*/ {BT_SPRINT}, /*A_MAP*/ {BT_MAP},
    /*A_RADIO*/ {BT_RADIO_TOGGLE}, /*A_RADIO_NEXT*/ {BT_RADIO}, /*A_WEAPON_NEXT*/ {BT_WEAPON_NEXT},
    /*A_WEAPON_PREV*/ {BT_WEAPON_PREV}, /*A_CONFIRM*/ {}, /*A_BACK*/ {}, /*A_MENU*/ {}};

const char* kActionNames[A_COUNT] = {"attack", "enter_car", "interact", "accelerate", "brake", "cover", "sprint",
                                     "map", "radio", "radio_next", "weapon_next", "weapon_prev", "confirm",
                                     "back", "menu"};

float axis(SDL_GameController* p, SDL_GameControllerAxis a) {
    return p ? std::clamp(SDL_GameControllerGetAxis(p, a) / 32767.0f, -1.0f, 1.0f) : 0.0f;
}

// Radial deadzone, rescaled so output starts at 0 just outside the zone.
void deadzone2(float* x, float* y, float dz) {
    const float m = std::sqrt(*x * *x + *y * *y);
    if (m <= dz || m == 0) { *x = *y = 0; return; }
    const float k = std::min(1.0f, (m - dz) / (1.0f - dz)) / m;
    *x *= k;
    *y *= k;
}

int button_by_name(const std::string& n) {
    static const struct { const char* n; SDL_GameControllerButton b; } t[] = {
        {"a", SDL_CONTROLLER_BUTTON_A}, {"b", SDL_CONTROLLER_BUTTON_B}, {"x", SDL_CONTROLLER_BUTTON_X},
        {"y", SDL_CONTROLLER_BUTTON_Y}, {"lb", SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
        {"rb", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER}, {"start", SDL_CONTROLLER_BUTTON_START},
        {"back", SDL_CONTROLLER_BUTTON_BACK}, {"up", SDL_CONTROLLER_BUTTON_DPAD_UP},
        {"down", SDL_CONTROLLER_BUTTON_DPAD_DOWN}, {"left", SDL_CONTROLLER_BUTTON_DPAD_LEFT},
        {"right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT}};
    for (const auto& e : t)
        if (n == e.n) return e.b;
    return -1;
}
}  // namespace

void Input::init(const GameCalls& g, int game_w, int game_h, const settings::Settings& s) {
    g_ = g;
    game_w_ = game_w;
    game_h_ = game_h;
    deadzone_ = std::clamp(s.stick_deadzone, 0.0f, 0.9f);
    mouse_look_ = s.mouse_look;
    mouse_sens_ = std::clamp(s.mouse_sensitivity, 0.05f, 10.0f);
    invert_y_ = s.invert_camera_y;
    set_viewport(0, 0, game_w, game_h);
    cursor_x_ = game_w * 0.5f;
    cursor_y_ = game_h * 0.5f;
    for (int i = 0; i < SDL_NumJoysticks() && !pad_; ++i)
        if (SDL_IsGameController(i)) pad_ = SDL_GameControllerOpen(i);
    if (pad_) LOG("input: controller \"%s\"", SDL_GameControllerName(pad_));
}

void Input::apply_settings(const settings::Settings& s) {
    deadzone_ = std::clamp(s.stick_deadzone, 0.0f, 0.9f);
    mouse_look_ = s.mouse_look;
    mouse_sens_ = std::clamp(s.mouse_sensitivity, 0.05f, 10.0f);
    invert_y_ = s.invert_camera_y;
}

void Input::release_all() {
    for (int id = P_STICK; id <= P_LAST; ++id) set_vtouch(id, false, 0, 0);
    if (mouse_down_) {
        touch(0, vt_[P_MOUSE].x, vt_[P_MOUSE].y, P_MOUSE);
        mouse_down_ = false;
    }
    act_ptr_.fill(0);
    tap_left_.fill(0);
    tap_frames_.fill(0);
    wheel_next_ = wheel_prev_ = 0;
    cursor_pressed_ = false;
    rmb_down_ = false;
    rmb_dx_ = rmb_dy_ = 0;
    key_edge(AK_BACK, false, &prev_back_);
    key_edge(AK_MENU, false, &prev_menu_);
    if (tilting_) {
        mods::set_virtual_steering(false, 0);
        tilting_ = false;
        steer_ = 0;
    }
}

void Input::suppress_held() { suppress_ = true; }

void Input::update_mouse_lock(bool allowed) {
    // Locked while playing with keyboard+mouse in gameplay; released in menus/cutscenes, the
    // settings menu and when the window isn't focused (SDL relative mode hides and confines the
    // cursor and reports raw motion).
    const bool want = mouse_look_ && allowed && gameplay_ && last_device_ != Device::Pad;
    if (want == mouse_locked_) return;
    if (!want && mouse_locked_ && lmb_attack_) lmb_attack_ = false;
    mouse_locked_ = want;
    SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
    if (want) {
        if (mouse_down_) {  // a click-drag that started as a touch: lift it
            touch(0, vt_[P_MOUSE].x, vt_[P_MOUSE].y, P_MOUSE);
            mouse_down_ = false;
        }
        rmb_down_ = false;
        rmb_dx_ = rmb_dy_ = 0;
    }
    LOG("input: mouse %s", want ? "locked (mouse-look)" : "released");
}

void Input::set_viewport(int x, int y, int w, int h) {
    vp_x_ = x; vp_y_ = y; vp_w_ = w; vp_h_ = h;
}

bool Input::to_game(int wx, int wy, int* gx, int* gy) const {
    if (vp_w_ <= 0 || vp_h_ <= 0) return false;
    const int x = (wx - vp_x_) * game_w_ / vp_w_, y = (wy - vp_y_) * game_h_ / vp_h_;
    *gx = std::clamp(x, 0, game_w_ - 1);
    *gy = std::clamp(y, 0, game_h_ - 1);
    return wx >= vp_x_ && wy >= vp_y_ && wx < vp_x_ + vp_w_ && wy < vp_y_ + vp_h_;
}

void Input::touch(int action, int x, int y, int pointer) {
    // (env, cls, action, x, y, jlong pointerId): y is the first stack slot, the jlong is 8-byte aligned.
    g_.rt->main_cpu().call(g_.rt->lib_func(kTouch), {g_.env, g_.cls_surface, uint32_t(action), uint32_t(x)},
                           {uint32_t(y), 0, uint32_t(pointer), 0});
}

void Input::key(int code, bool down) {
    g_.rt->main_cpu().call(g_.rt->lib_func(down ? kKeyDown : kKeyUp), {g_.env, g_.cls_g2, uint32_t(code)});
}

void Input::accelerometer(float x, float y, float z) {
    // (env, cls, float x, float y, float z), soft-float: x/y in r2/r3, z on the stack.
    uint32_t bx, by, bz;
    std::memcpy(&bx, &x, 4);
    std::memcpy(&by, &y, 4);
    std::memcpy(&bz, &z, 4);
    g_.rt->main_cpu().call(g_.rt->lib_func("Java_com_gameloft_android_GAND_GloftGMHP_ML_Gangster2_nativeAccelerometer"),
                           {g_.env, g_.cls_g2, bx, by}, {bz});
}

void Input::set_vtouch(int id, bool down, int x, int y) {
    VTouch& t = vt_[id];
    x = std::clamp(x, 0, game_w_ - 1);
    y = std::clamp(y, 0, game_h_ - 1);
    if (down && !t.down) touch(1, x, y, id);
    else if (down && (x != t.x || y != t.y)) touch(2, x, y, id);
    else if (!down && t.down) touch(0, t.x, t.y, id);
    if (down) t = {true, x, y};
    else t.down = false;
}

void Input::key_edge(int code, bool now, bool* prev) {
    if (now != *prev) key(code, now);
    *prev = now;
}

// ---- events --------------------------------------------------------------------------------------
void Input::handle(const SDL_Event& ev) {
    int gx, gy;
    if (mouse_locked_) {  // mouse-look: motion turns the camera, left click attacks
        switch (ev.type) {
            case SDL_MOUSEMOTION:
                rmb_dx_ += float(ev.motion.xrel) * game_w_ / std::max(vp_w_, 1);
                rmb_dy_ += float(ev.motion.yrel) * game_h_ / std::max(vp_h_, 1);
                return;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) lmb_attack_ = ev.type == SDL_MOUSEBUTTONDOWN;
                last_device_ = Device::Keyboard;
                return;
            default: break;
        }
    }
    switch (ev.type) {
        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT && to_game(ev.button.x, ev.button.y, &gx, &gy)) {
                mouse_down_ = true;
                last_device_ = Device::Touch;
                cursor_on_ = false;
                touch(1, gx, gy, P_MOUSE);
                vt_[P_MOUSE].x = gx;  // remembered for release_all()
                vt_[P_MOUSE].y = gy;
            } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                rmb_down_ = true;
                last_device_ = Device::Keyboard;  // mouse-look = keyboard+mouse play
            }
            break;
        case SDL_MOUSEMOTION:
            if (mouse_down_) {
                to_game(ev.motion.x, ev.motion.y, &gx, &gy);
                touch(2, gx, gy, P_MOUSE);
                vt_[P_MOUSE].x = gx;
                vt_[P_MOUSE].y = gy;
            }
            if (rmb_down_) {
                // window pixels -> game pixels
                rmb_dx_ += float(ev.motion.xrel) * game_w_ / std::max(vp_w_, 1);
                rmb_dy_ += float(ev.motion.yrel) * game_h_ / std::max(vp_h_, 1);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT && mouse_down_) {
                mouse_down_ = false;
                to_game(ev.button.x, ev.button.y, &gx, &gy);
                touch(0, gx, gy, P_MOUSE);
            } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                rmb_down_ = false;
            }
            break;
        case SDL_MOUSEWHEEL:
            if (ev.wheel.y > 0) wheel_next_ = std::min(wheel_next_ + 1, 3);  // queue notches, a few at most
            else if (ev.wheel.y < 0) wheel_prev_ = std::min(wheel_prev_ + 1, 3);
            break;
        case SDL_KEYDOWN:
            if (!ev.key.repeat) last_device_ = Device::Keyboard;
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (!pad_) {
                pad_ = SDL_GameControllerOpen(ev.cdevice.which);
                if (pad_) LOG("input: controller connected: \"%s\"", SDL_GameControllerName(pad_));
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (pad_ && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad_)) == ev.cdevice.which) {
                SDL_GameControllerClose(pad_);
                pad_ = nullptr;
                LOG("input: controller disconnected");
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            last_device_ = Device::Pad;
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (std::abs(ev.caxis.value) > 12000) last_device_ = Device::Pad;
            break;
        default: break;
    }
}

// ---- per-frame -----------------------------------------------------------------------------------
GameInput Input::gather(double dt) {
    GameInput gi;
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    auto pb = [&](SDL_GameControllerButton b) { return spad_[b] || (pad_ && SDL_GameControllerGetButton(pad_, b)); };
    auto k = [&](SDL_Scancode s) { return ks[s] != 0; };

    // movement: left stick (real + scripted) and WASD / arrows
    float lx = axis(pad_, SDL_CONTROLLER_AXIS_LEFTX) + slx_, ly = axis(pad_, SDL_CONTROLLER_AXIS_LEFTY) + sly_;
    deadzone2(&lx, &ly, deadzone_);
    float kx = float(int(k(SDL_SCANCODE_D) || k(SDL_SCANCODE_RIGHT)) - int(k(SDL_SCANCODE_A) || k(SDL_SCANCODE_LEFT)));
    float ky = float(int(k(SDL_SCANCODE_S) || k(SDL_SCANCODE_DOWN)) - int(k(SDL_SCANCODE_W) || k(SDL_SCANCODE_UP)));
    if (driving_) ky = 0;  // W/S are throttle/brake in a car; the stick only steers
    if (kx != 0 && ky != 0) { kx *= 0.7071f; ky *= 0.7071f; }
    gi.mx = std::clamp(lx + kx, -1.0f, 1.0f);
    gi.my = std::clamp(ly + ky, -1.0f, 1.0f);

    // camera: right stick
    float rx = axis(pad_, SDL_CONTROLLER_AXIS_RIGHTX) + srx_, ry = axis(pad_, SDL_CONTROLLER_AXIS_RIGHTY) + sry_;
    deadzone2(&rx, &ry, deadzone_);
    gi.cx = rx;
    gi.cy = invert_y_ ? -ry : ry;
    const bool rt = std::max(axis(pad_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT), srt_) > 0.3f;
    const bool lt = std::max(axis(pad_, SDL_CONTROLLER_AXIS_TRIGGERLEFT), slt_) > 0.3f;

    // controller
    if (driving_) {
        gi.act[A_ACCELERATE] = rt;
        gi.act[A_BRAKE] = lt;
        gi.act[A_ATTACK] = pb(SDL_CONTROLLER_BUTTON_B);  // drive-by
    } else {
        gi.act[A_ATTACK] = rt;
        gi.act[A_COVER] = lt || pb(SDL_CONTROLLER_BUTTON_B);
    }
    gi.act[A_WEAPON_NEXT] = pb(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    gi.act[A_WEAPON_PREV] = pb(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    gi.act[A_SPRINT] = pb(SDL_CONTROLLER_BUTTON_A);
    gi.act[A_INTERACT] = pb(SDL_CONTROLLER_BUTTON_X);
    gi.act[A_ENTER_CAR] = pb(SDL_CONTROLLER_BUTTON_Y);
    gi.act[A_MAP] = pb(SDL_CONTROLLER_BUTTON_BACK);
    gi.act[A_RADIO] = pb(SDL_CONTROLLER_BUTTON_DPAD_UP);
    gi.act[A_RADIO_NEXT] = pb(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    gi.act[A_CONFIRM] = pb(SDL_CONTROLLER_BUTTON_A);
    gi.act[A_BACK] = pb(SDL_CONTROLLER_BUTTON_START) || (!gameplay_ && pb(SDL_CONTROLLER_BUTTON_B));

    // keyboard
    gi.act[A_ATTACK] |= k(SDL_SCANCODE_SPACE) || k(SDL_SCANCODE_LCTRL) || lmb_attack_;
    gi.act[A_SPRINT] |= k(SDL_SCANCODE_LSHIFT) || k(SDL_SCANCODE_RSHIFT);
    gi.act[A_ENTER_CAR] |= k(SDL_SCANCODE_F);
    gi.act[A_INTERACT] |= k(SDL_SCANCODE_E);
    gi.act[A_COVER] |= k(SDL_SCANCODE_Q);
    gi.act[A_MAP] |= k(SDL_SCANCODE_M);
    gi.act[A_RADIO] |= k(SDL_SCANCODE_R);
    gi.act[A_RADIO_NEXT] |= k(SDL_SCANCODE_T);
    gi.act[A_WEAPON_PREV] |= k(SDL_SCANCODE_1);
    gi.act[A_WEAPON_NEXT] |= k(SDL_SCANCODE_2);
    // Mouse wheel: each notch is one press (one frame "down" starts a one-shot tap), spaced so the
    // tap ends and the button is released before the next notch presses again.
    wheel_cooldown_ = std::max(0.0, wheel_cooldown_ - dt);
    if (wheel_cooldown_ == 0 && (wheel_next_ > 0 || wheel_prev_ > 0)) {
        if (wheel_next_ > 0) { gi.act[A_WEAPON_NEXT] = true; --wheel_next_; }
        else { gi.act[A_WEAPON_PREV] = true; --wheel_prev_; }
        wheel_cooldown_ = kTapSeconds + kTapGapSeconds;
    }
    gi.act[A_ACCELERATE] |= driving_ && (k(SDL_SCANCODE_W) || k(SDL_SCANCODE_UP));
    gi.act[A_BRAKE] |= driving_ && (k(SDL_SCANCODE_S) || k(SDL_SCANCODE_DOWN));
    gi.act[A_CONFIRM] |= k(SDL_SCANCODE_RETURN) || k(SDL_SCANCODE_KP_ENTER);
    gi.act[A_BACK] |= k(SDL_SCANCODE_ESCAPE) || k(SDL_SCANCODE_BACKSPACE);
    gi.act[A_MENU] = k(SDL_SCANCODE_TAB);

    // menus: the d-pad moves the cursor too
    if (!gameplay_) {
        gi.mx += float(int(pb(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) - int(pb(SDL_CONTROLLER_BUTTON_DPAD_LEFT)));
        gi.my += float(int(pb(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) - int(pb(SDL_CONTROLLER_BUTTON_DPAD_UP)));
        gi.mx = std::clamp(gi.mx, -1.0f, 1.0f);
        gi.my = std::clamp(gi.my, -1.0f, 1.0f);
    }
    return gi;
}

void Input::drive_gameplay(const std::vector<mods::Control>& controls, const GameInput& gi, double dt) {
    auto find_type = [&](std::initializer_list<int> types) -> const mods::Control* {
        for (int t : types)
            for (const auto& c : controls)
                if (c.visible && c.type == t) return &c;
        return nullptr;
    };

    // movement: hold a touch on the v-stick, offset by the deflection
    const mods::Control* stick = nullptr;
    const mods::Control* steer = nullptr;  // on-screen wheel / slider (non-tilt steering modes)
    for (const auto& c : controls) {
        if (c.visible && !std::strcmp(c.kind, "AnalogStick")) stick = &c;
        if (c.visible && (!std::strcmp(c.kind, "Wheel") || !std::strcmp(c.kind, "SlideControl"))) steer = &c;
    }
    const bool tilt = driving_ && mods::steering_mode(*g_.rt) == mods::kSteerTilt;
    if (tilt) {
        // Tilt steering: the stick / keys set the steering directly (mods::set_virtual_steering);
        // a neutral accelerometer reading (phone level in landscape) keeps the game's handler running.
        // Keys are digital, so the steering slews (full lock in ~0.15 s) instead of snapping.
        const float step = float(dt) * 7.0f;
        steer_ += std::clamp(gi.mx - steer_, -step, step);
        mods::set_virtual_steering(true, steer_);
        accelerometer(-5.0f, 0.0f, 8.5f);
        tilting_ = true;
    } else if (tilting_) {
        mods::set_virtual_steering(false, 0);
        steer_ = 0;
        tilting_ = false;
    }
    if (driving_ && steer && !tilt) {
        stick = nullptr;
        const float cx = (steer->x0 + steer->x1) * 0.5f, cy = (steer->y0 + steer->y1) * 0.5f;
        const float r = (steer->x1 - steer->x0) * 0.4f;
        if (gi.mx == 0) set_vtouch(P_STICK, false, 0, 0);
        else if (!vt_[P_STICK].down) set_vtouch(P_STICK, true, int(cx), int(cy));
        else set_vtouch(P_STICK, true, int(std::lround(cx + gi.mx * r)), int(cy));
    } else if (stick && (gi.mx != 0 || gi.my != 0)) {
        const float cx = (stick->x0 + stick->x1) * 0.5f, cy = (stick->y0 + stick->y1) * 0.5f;
        const float r = std::min(stick->x1 - stick->x0, stick->y1 - stick->y0) * 0.4f;
        // Like a finger: land on the knob first, then drag (the stick only grabs touches that start on it).
        if (!vt_[P_STICK].down) set_vtouch(P_STICK, true, int(cx), int(cy));
        else set_vtouch(P_STICK, true, int(std::lround(cx + gi.mx * r)), int(std::lround(cy + gi.my * r)));
    } else {
        set_vtouch(P_STICK, false, 0, 0);
    }

    // actions: touch the visible button of the matching type. Held actions (attack, sprint, pedals,
    // cover) hold the touch; one-shot actions are a short tap on the press edge, so the button
    // fires once even if it disappears while the key is still held.
    for (int a = 0; a < A_COUNT; ++a) {
        if (!kActionTypes[a].size()) continue;
        const bool held = a == A_ATTACK || a == A_SPRINT || a == A_ACCELERATE || a == A_BRAKE || a == A_COVER;
        bool want = gi.act[a];
        if (!held) {
            // One tap per press: on the press edge if the button is showing, else as soon as it
            // appears while still held ("hold Y to get into the car once in range").
            const bool avail = find_type(kActionTypes[a]) != nullptr;
            if (!gi.act[a]) fired_[a] = false;
            else if (!fired_[a] && avail) { tap_left_[a] = kTapSeconds; tap_frames_[a] = 0; fired_[a] = true; }
            want = tap_left_[a] > 0 || (fired_[a] && tap_frames_[a] < kTapMinFrames);
            if (want) {
                tap_left_[a] = std::max(0.0, tap_left_[a] - dt);
                ++tap_frames_[a];
            }
        }
        const mods::Control* c = want ? find_type(kActionTypes[a]) : nullptr;
        int& p = act_ptr_[a];
        if (c) {
            if (!p) {  // grab a free pointer id
                for (int id = P_FIRST_BUTTON; id <= P_LAST && !p; ++id) {
                    bool used = vt_[id].down;
                    for (int b = 0; b < A_COUNT; ++b) used = used || act_ptr_[b] == id;
                    if (!used) p = id;
                }
                if (p) LOG("[input] %s -> button type %d (%s +%02x)", kActionNames[a], c->type, c->kind, c->field);
            }
            if (p) set_vtouch(p, true, int((c->x0 + c->x1) * 0.5f), int((c->y0 + c->y1) * 0.5f));
        } else if (p) {
            set_vtouch(p, false, 0, 0);
            p = 0;
        }
    }

    // camera: a continuous drag on empty screen space that re-grabs before it reaches a control
    const float speed = 700.0f * mouse_sens_;  // game pixels per second at full deflection
    const float dx = float(gi.cx * speed * dt) + rmb_dx_ * mouse_sens_;
    const float dy = float(gi.cy * speed * dt) + (invert_y_ ? -rmb_dy_ : rmb_dy_) * mouse_sens_;
    rmb_dx_ = rmb_dy_ = 0;
    const float ax = game_w_ * 0.62f, ay = game_h_ * 0.40f;  // anchor: right of center, above the buttons
    if (dx != 0 || dy != 0 || (rmb_down_ && vt_[P_CAMERA].down)) {
        if (!vt_[P_CAMERA].down) {
            cam_x_ = ax;
            cam_y_ = ay;
            set_vtouch(P_CAMERA, true, int(cam_x_), int(cam_y_));
        }
        cam_x_ += dx;
        cam_y_ += dy;
        if (std::abs(cam_x_ - ax) > game_w_ * 0.25f || std::abs(cam_y_ - ay) > game_h_ * 0.3f)
            set_vtouch(P_CAMERA, false, 0, 0);  // lift; grab again at the anchor next frame
        else
            set_vtouch(P_CAMERA, true, int(std::lround(cam_x_)), int(std::lround(cam_y_)));
    } else {
        set_vtouch(P_CAMERA, false, 0, 0);
    }
}

void Input::drive_menus(const GameInput& gi, double dt) {
    if (gi.mx != 0 || gi.my != 0) {
        cursor_on_ = true;
        cursor_x_ = std::clamp(cursor_x_ + float(gi.mx * 650.0 * dt), 0.0f, float(game_w_ - 1));
        cursor_y_ = std::clamp(cursor_y_ + float(gi.my * 650.0 * dt), 0.0f, float(game_h_ - 1));
        if (cursor_pressed_) set_vtouch(P_STICK, true, int(cursor_x_), int(cursor_y_));  // drag (sliders, lists)
    }
    if (gi.act[A_CONFIRM] && !cursor_pressed_) {
        cursor_on_ = true;
        cursor_pressed_ = true;
        set_vtouch(P_STICK, true, int(cursor_x_), int(cursor_y_));
    } else if (!gi.act[A_CONFIRM] && cursor_pressed_) {
        cursor_pressed_ = false;
        set_vtouch(P_STICK, false, 0, 0);
    }
}

void Input::update(double dt) {
    const std::vector<mods::Control> controls = mods::controls(*g_.rt, game_w_, game_h_);
    // Gameplay = the v-stick or a gameplay button is showing; driving = pedals are showing.
    bool now_gameplay = false, now_driving = false;
    for (const auto& c : controls) {
        if (!c.visible) continue;
        if (!std::strcmp(c.kind, "AnalogStick") || c.type >= 0) now_gameplay = true;
        if (c.type == BT_ACCEL || c.type == BT_BRAKE) now_driving = true;
    }
    if (now_gameplay != gameplay_ || now_driving != driving_) {
        for (int id = P_STICK; id <= P_LAST; ++id) set_vtouch(id, false, 0, 0);
        act_ptr_.fill(0);
        tap_left_.fill(0);
        tap_frames_.fill(0);  // (fired_ is kept: a still-held Y must not exit the car it just entered)
        mods::set_virtual_steering(false, 0);
        tilting_ = false;
        steer_ = 0;
        cursor_pressed_ = false;
        if (now_gameplay != gameplay_)
            LOG("[input] mode: %s", now_gameplay ? (now_driving ? "driving" : "on foot") : "menu cursor");
        else
            LOG("[input] mode: %s", now_driving ? "driving" : "on foot");
        if (now_driving) LOG("[input] steering mode %#x", mods::steering_mode(*g_.rt));
        gameplay_ = now_gameplay;
        driving_ = now_driving;
    }
    GameInput gi = gather(dt);
    if (suppress_) {  // after the settings menu closed: wait until everything is released
        bool any = gi.mx != 0 || gi.my != 0 || gi.cx != 0 || gi.cy != 0;
        for (bool b : gi.act) any = any || b;
        if (any) gi = GameInput{};
        else suppress_ = false;
    }
    if (gameplay_) {
        cursor_on_ = false;
        drive_gameplay(controls, gi, dt);
    } else {
        drive_menus(gi, dt);
    }
    // Esc / Start = Android BACK (pause / back out of menus), Tab = MENU.
    key_edge(AK_BACK, gi.act[A_BACK], &prev_back_);
    key_edge(AK_MENU, gi.act[A_MENU], &prev_menu_);
}

// ---- scripted input ------------------------------------------------------------------------------
bool Input::parse_script(const std::string& script, std::string* err) {
    std::stringstream ss(script);
    std::string item;
    while (std::getline(ss, item, ';')) {
        if (item.find_first_not_of(" \t") == std::string::npos) continue;
        const size_t colon = item.find(':');
        if (colon == std::string::npos) {
            *err = "bad input script item '" + item + "' (expected 'frame:cmd ...' or 't<sec>:cmd ...')";
            return false;
        }
        std::string when = item.substr(0, colon);
        while (!when.empty() && when.front() == ' ') when.erase(0, 1);
        Step s{-1, -1.0, "", "", 0, 0, false};
        if (!when.empty() && when[0] == 't') s.time = std::atof(when.c_str() + 1);
        else s.frame = std::atoi(when.c_str());
        std::stringstream is(item.substr(colon + 1));
        is >> s.cmd;
        if (s.cmd == "pad" || s.cmd == "stick") is >> s.a;
        if (s.cmd == "key" || s.cmd == "pad") is >> s.x;
        else is >> s.x >> s.y;
        const bool known = s.cmd == "tap" || s.cmd == "down" || s.cmd == "move" || s.cmd == "up" ||
                           s.cmd == "key" || s.cmd == "pad" || s.cmd == "stick";
        if (!known || is.fail()) {
            *err = "bad input script item '" + item + "'";
            return false;
        }
        if (s.cmd == "pad" && button_by_name(s.a) < 0 && s.a != "lt" && s.a != "rt") {
            *err = "unknown pad button '" + s.a + "'";
            return false;
        }
        if (s.cmd == "stick" && s.a != "l" && s.a != "r") {
            *err = "stick must be 'l' or 'r' in '" + item + "'";
            return false;
        }
        script_.push_back(s);
        Step rel = s;  // tap / key: release 3 frames or 0.1 s later
        if (rel.frame >= 0) rel.frame += 3;
        else rel.time += 0.1;
        if (s.cmd == "tap") { rel.cmd = "up"; script_.push_back(rel); }
        if (s.cmd == "key") { rel.cmd = "keyup"; script_.push_back(rel); }
    }
    return true;
}

void Input::run_script(int frame, double seconds) {
    // Steps run in order, at most one per frame, so a long frame (e.g. a loading hitch)
    // cannot collapse several steps into one frame.
    for (Step& s : script_) {
        if (s.done) continue;
        if (s.frame >= 0 ? s.frame > frame : seconds < s.time) break;
        s.done = true;
        LOG("[input] frame %d (%.1f s): %s %s %d %d", frame, seconds, s.cmd.c_str(), s.a.c_str(), s.x, s.y);
        if (s.cmd == "tap" || s.cmd == "down") touch(1, s.x, s.y, P_MOUSE);
        else if (s.cmd == "move") touch(2, s.x, s.y, P_MOUSE);
        else if (s.cmd == "up") touch(0, s.x, s.y, P_MOUSE);
        else if (s.cmd == "key") key(s.x, true);
        else if (s.cmd == "keyup") key(s.x, false);
        else if (s.cmd == "pad") {
            last_device_ = Device::Pad;
            if (s.a == "lt") slt_ = s.x ? 1.0f : 0.0f;
            else if (s.a == "rt") srt_ = s.x ? 1.0f : 0.0f;
            else spad_[button_by_name(s.a)] = s.x != 0;
        } else if (s.cmd == "stick") {
            last_device_ = Device::Pad;
            (s.a == "r" ? srx_ : slx_) = std::clamp(s.x, -100, 100) / 100.0f;
            (s.a == "r" ? sry_ : sly_) = std::clamp(s.y, -100, 100) / 100.0f;
        }
        break;
    }
}

}  // namespace input
