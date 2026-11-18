#pragma once
// Host input -> the game's input entry points.
//
//   touch: Java_..._GameGLSurfaceView_nativeOnTouch(env, cls, action, x, y, (jlong)pointerId)
//          action 1 = down, 2 = move, 0 = up (as GameGLSurfaceView.onTouchEvent sends them)
//          -> appOnTouch -> AndroidAddTouchEvent -> glf::TouchPad ring buffer
//   keys:  Java_..._Gangster2_nativeKeyDown/Up(env, cls, androidKeycode) -> appKeyPressed/Released
//
// All calls are made on the render thread between frames (glf queues touch events,
// so this matches the phone, where touches arrived from the UI thread).
#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rt { class Runtime; }

namespace input {

struct GameCalls {
    rt::Runtime* rt = nullptr;
    uint32_t env = 0, cls_surface = 0, cls_g2 = 0;
};

class Input {
public:
    void init(const GameCalls& g, int game_w, int game_h);
    void set_viewport(int x, int y, int w, int h);  // where the game image sits in the window

    void handle(const SDL_Event& ev);  // window events (mouse, keyboard)

    // Scripted input for automated runs: "600:tap 400 240; 900:key 4" (frame:command)
    bool parse_script(const std::string& script, std::string* err);
    void run_script(int frame);

    void touch(int action, int x, int y, int pointer);
    void key(int android_keycode, bool down);

private:
    bool to_game(int wx, int wy, int* gx, int* gy) const;

    GameCalls g_;
    int game_w_ = 800, game_h_ = 480;
    int vp_x_ = 0, vp_y_ = 0, vp_w_ = 800, vp_h_ = 480;
    bool mouse_down_ = false;
    struct Step { int frame; std::string cmd; int a, b; };
    std::vector<Step> script_;
};

}  // namespace input
