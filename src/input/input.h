#pragma once
// Host input -> the game.
//
// The game only understands touches (plus a few Android key codes). Mouse clicks are
// passed through as touches. Keyboard and game controllers drive an abstract GameInput
// state, which is turned into VIRTUAL TOUCHES on the controls the game currently shows
// (positions read live from the game's HUD objects, see game/mods.h):
//   move   -> a held touch on the v-stick, offset by the stick deflection
//   action -> a held touch on the visible VirtualButton of that ButtonType
//   camera -> a continuous drag on empty screen space (the game turns the camera on drags)
// When no gameplay controls are visible (menus, cutscenes) the pad drives a cursor instead.
// Because the game does all the context logic, this works the same with the touch HUD
// hidden, and the same on Windows and Android.
//
// Game entry points:
//   touch: GameGLSurfaceView.nativeOnTouch(env, cls, action 1=down 2=move 0=up, x, y, jlong id)
//   keys:  Gangster2.nativeKeyDown/Up(env, cls, androidKeycode)
#include <SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rt { class Runtime; }
namespace settings { struct Settings; }
namespace mods { struct Control; }

namespace input {

struct GameCalls {
    rt::Runtime* rt = nullptr;
    uint32_t env = 0, cls_surface = 0, cls_g2 = 0;
};

// Abstract actions (ButtonType values from OnFoot/DrivingControlHandler::handleVirtualButton).
enum Action {
    A_ATTACK, A_ENTER_CAR, A_INTERACT, A_ACCELERATE, A_BRAKE, A_COVER, A_SPRINT,
    A_MAP, A_RADIO, A_RADIO_NEXT, A_WEAPON_NEXT, A_WEAPON_PREV, A_AIM, A_AUTO_AIM,
    A_RELOAD, A_LOCK_TOGGLE, A_CONFIRM, A_BACK, A_MENU, A_COUNT
};

struct GameInput {
    float mx = 0, my = 0;   // movement / steering  (-1..1, y down)
    float cx = 0, cy = 0;   // camera (-1..1)
    bool act[A_COUNT] = {};
};

enum class Device { Touch, Keyboard, Pad };

class Input {
public:
    void init(const GameCalls& g, int game_w, int game_h, const settings::Settings& s);
    void set_viewport(int x, int y, int w, int h);  // where the game image sits in the window
    void set_window_size(int w, int h) { win_w_ = w; win_h_ = h; }  // for touch coordinates (0..1)

    void handle(const SDL_Event& ev);   // window / device events
    void update(double dt);             // once per frame, before nativeRender

    // Scripted input for automated runs (frame:cmd or t<seconds>:cmd), cmds:
    //   tap x y | down x y | move x y | up x y | key code
    //   pad <a|b|x|y|lb|rb|lt|rt|start|back|up|down|left|right> <0|1>
    //   stick <l|r> <x> <y>        (-100..100)
    bool parse_script(const std::string& script, std::string* err);
    void run_script(int frame, double seconds);

    void touch(int action, int x, int y, int pointer);
    void key(int android_keycode, bool down);

    void apply_settings(const settings::Settings& s);  // live: deadzone, camera speed, invert Y
    void release_all();      // lift every virtual touch / key (e.g. when the settings menu opens)
    void suppress_held();    // ignore keys/buttons already held until they are released
    // Mouse-look: lock the cursor (SDL relative mode) when allowed (window focused, settings
    // menu closed), playing with keyboard+mouse and the gameplay HUD is up. Call every frame.
    void update_mouse_lock(bool allowed);
    bool mouse_locked() const { return mouse_locked_; }

    Device last_device() const { return last_device_; }
    // Menu cursor (game coordinates) for the overlay; visible while a pad drives menus.
    bool cursor_visible() const { return cursor_on_; }
    float cursor_x() const { return cursor_x_; }
    float cursor_y() const { return cursor_y_; }
    bool gameplay_controls_visible() const { return gameplay_; }

private:
    bool to_game(int wx, int wy, int* gx, int* gy) const;
    void mouse_button(int button, bool down);  // mouse-look buttons (real or scripted)
    void mouse_wheel(int notches);             // + = away from the user (up), - = down
    void set_vtouch(int id, bool down, int x, int y);
    GameInput gather(double dt);
    void drive_gameplay(const std::vector<mods::Control>& controls, const GameInput& gi, double dt);
    void drive_menus(const GameInput& gi, double dt);
    void key_edge(int code, bool now, bool* prev);
    void accelerometer(float x, float y, float z);  // Gangster2.nativeAccelerometer (m/s^2, device axes)

    GameCalls g_;
    int game_w_ = 800, game_h_ = 480;
    int vp_x_ = 0, vp_y_ = 0, vp_w_ = 800, vp_h_ = 480;
    float deadzone_ = 0.2f, mouse_sens_ = 1.0f;
    bool invert_y_ = false;
    bool mouse_down_ = false, rmb_down_ = false;
    float rmb_dx_ = 0, rmb_dy_ = 0;
    Device last_device_ = Device::Touch;

    SDL_GameController* pad_ = nullptr;
    // scripted pad state (OR-ed with the real pad)
    bool spad_[SDL_CONTROLLER_BUTTON_MAX] = {};
    float slx_ = 0, sly_ = 0, srx_ = 0, sry_ = 0, slt_ = 0, srt_ = 0;

    struct VTouch { bool down = false; int x = 0, y = 0; };
    std::array<VTouch, 10> vt_{};           // pointer ids: 0 mouse, 1 stick, 2 camera, 3..9 buttons
    std::array<int, A_COUNT> act_ptr_{};     // pointer held for an action (0 = none)
    std::array<bool, A_COUNT> fired_{};      // the one-shot action already tapped during this press
    std::array<double, A_COUNT> tap_left_{};  // seconds left of a one-shot tap (see kTapSeconds)
    std::array<int, A_COUNT> tap_frames_{};   // frames since that tap started (at least kTapMinFrames)
    float cam_x_ = 0, cam_y_ = 0;            // current camera drag point
    bool gameplay_ = false, driving_ = false;
    bool tilting_ = false;                   // virtual steering is active (tilt steering mode)
    float steer_ = 0;                        // current virtual steering (-1..1, slewed)
    int wheel_next_ = 0, wheel_prev_ = 0;    // mouse-wheel notches not yet sent (weapon next/prev)
    double wheel_cooldown_ = 0;              // until the next notch may press (tap + release gap)
    bool cursor_on_ = false, cursor_pressed_ = false;
    float cursor_x_ = 0, cursor_y_ = 0;
    bool prev_back_ = false, prev_menu_ = false;
    bool suppress_ = false;
    bool mouse_look_ = true, mouse_locked_ = false, lmb_attack_ = false;
    bool rmb_held_ = false, mmb_held_ = false;  // mouse-look: right = aim (on foot) / fire (car), middle = lock toggle
    bool mmb_click_ = false;                    // a middle click not yet handled (lock on / off)
    bool weapon_prev_ = false, weapon_next_ = false;  // last frame's weapon actions (press edges)
    bool reload_was_ = false, lock_toggle_was_ = false;
    bool skey_[SDL_NUM_SCANCODES] = {};         // scripted keyboard state (OR-ed with the real keys)
    // Real fingers (touchscreens): SDL finger id per game pointer id 0..4.
    static constexpr int kFingers = 5;
    SDL_FingerID finger_[kFingers] = {};
    bool finger_down_[kFingers] = {};
    int win_w_ = 1280, win_h_ = 720;
    SDL_Sensor* accel_ = nullptr;               // the device accelerometer (Android)
    bool tilt_handheld_ = true, tilt_invert_ = false;

    struct Step { int frame; double time; std::string cmd; std::string a; int x, y; bool done; };
    std::vector<Step> script_;
};

}  // namespace input
