#include "input/input.h"

#include <cstdio>
#include <sstream>

#include "core/log.h"
#include "rt/runtime.h"

namespace input {

namespace {
const char* kTouch = "Java_com_gameloft_android_GAND_GloftGMHP_ML_GameGLSurfaceView_nativeOnTouch";
const char* kKeyDown = "Java_com_gameloft_android_GAND_GloftGMHP_ML_Gangster2_nativeKeyDown";
const char* kKeyUp = "Java_com_gameloft_android_GAND_GloftGMHP_ML_Gangster2_nativeKeyUp";

// Android KeyEvent keycodes
enum { AK_BACK = 4, AK_MENU = 82, AK_ENTER = 66, AK_DPAD_UP = 19, AK_DPAD_DOWN = 20, AK_DPAD_LEFT = 21,
       AK_DPAD_RIGHT = 22, AK_DPAD_CENTER = 23, AK_SPACE = 62 };
}  // namespace

void Input::init(const GameCalls& g, int game_w, int game_h) {
    g_ = g;
    game_w_ = game_w;
    game_h_ = game_h;
    set_viewport(0, 0, game_w, game_h);
}

void Input::set_viewport(int x, int y, int w, int h) {
    vp_x_ = x; vp_y_ = y; vp_w_ = w; vp_h_ = h;
}

bool Input::to_game(int wx, int wy, int* gx, int* gy) const {
    if (vp_w_ <= 0 || vp_h_ <= 0) return false;
    int x = (wx - vp_x_) * game_w_ / vp_w_, y = (wy - vp_y_) * game_h_ / vp_h_;
    *gx = x < 0 ? 0 : x >= game_w_ ? game_w_ - 1 : x;
    *gy = y < 0 ? 0 : y >= game_h_ ? game_h_ - 1 : y;
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

void Input::handle(const SDL_Event& ev) {
    int gx, gy;
    switch (ev.type) {
        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT && to_game(ev.button.x, ev.button.y, &gx, &gy)) {
                mouse_down_ = true;
                touch(1, gx, gy, 0);
            }
            break;
        case SDL_MOUSEMOTION:
            if (mouse_down_) {
                to_game(ev.motion.x, ev.motion.y, &gx, &gy);
                touch(2, gx, gy, 0);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT && mouse_down_) {
                mouse_down_ = false;
                to_game(ev.button.x, ev.button.y, &gx, &gy);
                touch(0, gx, gy, 0);
            }
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            if (ev.key.repeat) break;
            int code = 0;
            switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: case SDLK_BACKSPACE: code = AK_BACK; break;
                case SDLK_TAB: code = AK_MENU; break;
                case SDLK_RETURN: code = AK_ENTER; break;
                case SDLK_UP: code = AK_DPAD_UP; break;
                case SDLK_DOWN: code = AK_DPAD_DOWN; break;
                case SDLK_LEFT: code = AK_DPAD_LEFT; break;
                case SDLK_RIGHT: code = AK_DPAD_RIGHT; break;
                case SDLK_SPACE: code = AK_SPACE; break;
                default: break;
            }
            if (code) key(code, ev.type == SDL_KEYDOWN);
            break;
        }
        default: break;
    }
}

bool Input::parse_script(const std::string& script, std::string* err) {
    std::stringstream ss(script);
    std::string item;
    while (std::getline(ss, item, ';')) {
        size_t colon = item.find(':');
        if (colon == std::string::npos) {
            *err = "bad input script item '" + item + "' (expected 'frame:cmd ...' or 't<sec>:cmd ...')";
            return false;
        }
        std::string when = item.substr(0, colon);
        while (!when.empty() && when.front() == ' ') when.erase(0, 1);
        Step s{-1, -1.0, "", 0, 0, false};
        if (!when.empty() && when[0] == 't') s.time = std::atof(when.c_str() + 1);
        else s.frame = std::atoi(when.c_str());
        char cmd[16] = {};
        int n = std::sscanf(item.c_str() + colon + 1, " %15s %d %d", cmd, &s.a, &s.b);
        if (n < 2) {
            *err = "bad input script item '" + item + "' (expected 'tap x y' or 'key code')";
            return false;
        }
        s.cmd = cmd;
        if (s.cmd != "tap" && s.cmd != "key" && s.cmd != "down" && s.cmd != "up" && s.cmd != "move") {
            *err = "unknown input command '" + s.cmd + "'";
            return false;
        }
        script_.push_back(s);
        // release 3 frames / 0.1 s later
        Step rel = s;
        if (rel.frame >= 0) rel.frame += 3; else rel.time += 0.1;
        if (s.cmd == "tap") { rel.cmd = "up"; script_.push_back(rel); }
        if (s.cmd == "key") { rel.cmd = "keyup"; script_.push_back(rel); }
    }
    return true;
}

void Input::run_script(int frame, double seconds) {
    // Steps run in order, at most one per frame, so a long frame (e.g. a loading hitch)
    // cannot collapse several taps into one frame.
    for (Step& s : script_) {
        if (s.done) continue;
        if (s.frame >= 0 ? s.frame > frame : seconds < s.time) break;
        s.done = true;
        LOG("[input] frame %d (%.1f s): %s %d %d", frame, seconds, s.cmd.c_str(), s.a, s.b);
        if (s.cmd == "tap" || s.cmd == "down") touch(1, s.a, s.b, 0);
        else if (s.cmd == "move") touch(2, s.a, s.b, 0);
        else if (s.cmd == "up") touch(0, s.a, s.b, 0);
        else if (s.cmd == "key") key(s.a, true);
        else if (s.cmd == "keyup") key(s.a, false);
        break;
    }
}

}  // namespace input
