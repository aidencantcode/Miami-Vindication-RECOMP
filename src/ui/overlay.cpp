#include "ui/overlay.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/log.h"
#include "settings/settings.h"

namespace ui {

namespace {

bool g_ready = false;
bool g_open = false;
SDL_Window* g_win = nullptr;
bool g_l3 = false, g_r3 = false;

struct Toast { std::string text; std::chrono::steady_clock::time_point until; };
std::vector<Toast> g_toasts;

// ---- minimal SDL2 platform glue for ImGui -------------------------------------------------------
// (vcpkg's ImGui no longer ships an SDL2 backend.) Only what a settings menu needs: mouse,
// keyboard navigation, controller navigation, display size and frame time.
std::chrono::steady_clock::time_point g_last_frame;

ImGuiKey imgui_key(SDL_Keycode k) {
    switch (k) {
        case SDLK_TAB: return ImGuiKey_Tab;
        case SDLK_LEFT: return ImGuiKey_LeftArrow;
        case SDLK_RIGHT: return ImGuiKey_RightArrow;
        case SDLK_UP: return ImGuiKey_UpArrow;
        case SDLK_DOWN: return ImGuiKey_DownArrow;
        case SDLK_PAGEUP: return ImGuiKey_PageUp;
        case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
        case SDLK_HOME: return ImGuiKey_Home;
        case SDLK_END: return ImGuiKey_End;
        case SDLK_SPACE: return ImGuiKey_Space;
        case SDLK_RETURN: return ImGuiKey_Enter;
        case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
        case SDLK_ESCAPE: return ImGuiKey_Escape;
        case SDLK_BACKSPACE: return ImGuiKey_Backspace;
        case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
        case SDLK_RCTRL: return ImGuiKey_RightCtrl;
        case SDLK_LSHIFT: return ImGuiKey_LeftShift;
        case SDLK_RSHIFT: return ImGuiKey_RightShift;
        case SDLK_LALT: return ImGuiKey_LeftAlt;
        case SDLK_RALT: return ImGuiKey_RightAlt;
        default: return ImGuiKey_None;
    }
}

ImGuiKey imgui_pad_button(Uint8 b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_A: return ImGuiKey_GamepadFaceDown;   // activate
        case SDL_CONTROLLER_BUTTON_B: return ImGuiKey_GamepadFaceRight;  // cancel
        case SDL_CONTROLLER_BUTTON_X: return ImGuiKey_GamepadFaceLeft;
        case SDL_CONTROLLER_BUTTON_Y: return ImGuiKey_GamepadFaceUp;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return ImGuiKey_GamepadL1;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return ImGuiKey_GamepadR1;
        case SDL_CONTROLLER_BUTTON_BACK: return ImGuiKey_GamepadBack;
        default: return ImGuiKey_None;
    }
}

void platform_event(const SDL_Event& ev) {
    ImGuiIO& io = ImGui::GetIO();
    switch (ev.type) {
        case SDL_MOUSEMOTION: io.AddMousePosEvent(float(ev.motion.x), float(ev.motion.y)); break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            const int b = ev.button.button == SDL_BUTTON_LEFT ? 0 : ev.button.button == SDL_BUTTON_RIGHT ? 1
                        : ev.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
            io.AddMousePosEvent(float(ev.button.x), float(ev.button.y));
            if (b >= 0) io.AddMouseButtonEvent(b, ev.type == SDL_MOUSEBUTTONDOWN);
            break;
        }
        case SDL_MOUSEWHEEL: io.AddMouseWheelEvent(float(ev.wheel.x), float(ev.wheel.y)); break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            const SDL_Keymod m = SDL_Keymod(ev.key.keysym.mod);
            io.AddKeyEvent(ImGuiMod_Ctrl, (m & KMOD_CTRL) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (m & KMOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (m & KMOD_ALT) != 0);
            if (const ImGuiKey k = imgui_key(ev.key.keysym.sym); k != ImGuiKey_None)
                io.AddKeyEvent(k, ev.type == SDL_KEYDOWN);
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            if (const ImGuiKey k = imgui_pad_button(ev.cbutton.button); k != ImGuiKey_None)
                io.AddKeyEvent(k, ev.type == SDL_CONTROLLERBUTTONDOWN);
            break;
        case SDL_CONTROLLERAXISMOTION: {
            const float v = ev.caxis.value / 32767.0f;
            auto pair = [&](ImGuiKey neg, ImGuiKey pos) {
                io.AddKeyAnalogEvent(neg, v < -0.3f, v < 0 ? -v : 0.0f);
                io.AddKeyAnalogEvent(pos, v > 0.3f, v > 0 ? v : 0.0f);
            };
            if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) pair(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight);
            if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) pair(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown);
            break;
        }
        default: break;
    }
}

void platform_new_frame() {
    ImGuiIO& io = ImGui::GetIO();
    int w = 0, h = 0, dw = 0, dh = 0;
    SDL_GetWindowSize(g_win, &w, &h);
    SDL_GL_GetDrawableSize(g_win, &dw, &dh);
    io.DisplaySize = ImVec2(float(w), float(h));
    if (w > 0 && h > 0) io.DisplayFramebufferScale = ImVec2(float(dw) / w, float(dh) / h);
    const auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::max(1e-4f, std::chrono::duration<float>(now - g_last_frame).count());
    if (io.DeltaTime > 0.5f) io.DeltaTime = 1.0f / 60;
    g_last_frame = now;
}

// A combo box over string values.
bool combo_str(const char* label, std::string* v, std::initializer_list<std::pair<const char*, const char*>> items) {
    const char* shown = v->c_str();
    for (const auto& [val, name] : items)
        if (*v == val) shown = name;
    bool changed = false;
    if (ImGui::BeginCombo(label, shown)) {
        for (const auto& [val, name] : items) {
            const bool sel = *v == val;
            if (ImGui::Selectable(name, sel)) { changed = *v != val; *v = val; }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool combo_int(const char* label, int* v, std::initializer_list<std::pair<int, const char*>> items) {
    const char* shown = "custom";
    for (const auto& [val, name] : items)
        if (*v == val) shown = name;
    bool changed = false;
    if (ImGui::BeginCombo(label, shown)) {
        for (const auto& [val, name] : items) {
            const bool sel = *v == val;
            if (ImGui::Selectable(name, sel)) { changed = *v != val; *v = val; }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void restart_note() {
    ImGui::SameLine();
    ImGui::TextDisabled("(restart)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Takes effect the next time the game starts.");
}

bool settings_window(settings::Settings& s, const FrameStats& st) {
    bool ch = false;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->WorkSize.x * 0.9f, ImGui::GetFontSize() * 34),
                                    std::min(vp->WorkSize.y * 0.9f, ImGui::GetFontSize() * 30)),
                             ImGuiCond_Always);
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    ImGui::TextDisabled("Game %dx%d, rendered at %dx%d   %.0f fps, game %.1f ms/frame", st.game_w, st.game_h,
                        st.render_w, st.render_h, st.fps, st.game_ms);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Display")) {
            ch |= combo_str("Window", &s.window_mode,
                            {{"windowed", "Windowed"}, {"borderless", "Borderless fullscreen"}, {"fullscreen", "Exclusive fullscreen"}});
            ch |= ImGui::Checkbox("VSync", &s.vsync);
            ch |= combo_int("FPS limit", &s.fps_limit,
                            {{-1, "Display refresh rate"}, {30, "30"}, {60, "60"}, {90, "90"}, {120, "120"}, {144, "144"},
                             {165, "165"}, {240, "240"},
                             {360, "360"}, {0, "Unlimited"}});
            if (s.vsync) ImGui::TextDisabled("With VSync on, the display's refresh rate also caps the frame rate.");
            ch |= combo_int("Render scale", &s.render_scale,
                            {{0, "Auto (match the window)"}, {1, "1x (480p, original)"}, {2, "2x (960p)"}, {3, "3x (1440p)"},
                             {4, "4x (1920p)"}, {5, "5x (2400p)"}, {6, "6x (2880p)"}});
            ch |= combo_int("Anti-aliasing", &s.msaa, {{0, "Off"}, {2, "MSAA 2x"}, {4, "MSAA 4x"}, {8, "MSAA 8x"}});
            ch |= combo_str("Scaling", &s.fit, {{"aspect", "Keep aspect"}, {"stretch", "Stretch"}, {"integer", "Integer"}});
            ch |= ImGui::Checkbox("Smooth scaling", &s.smooth_scaling);
            ch |= combo_str("Aspect ratio", &s.aspect,
                            {{"auto", "Auto (window)"}, {"4:3", "4:3"}, {"5:3", "5:3 (original 800x480)"},
                             {"16:10", "16:10"}, {"16:9", "16:9"}, {"21:9", "21:9"}});
            restart_note();
            ch |= ImGui::Checkbox("Show FPS", &s.show_fps);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Graphics")) {
            ch |= combo_str("Quality", &s.quality,
                            {{"phone", "Phone (exactly as on the device)"}, {"low", "Low"}, {"high", "High"},
                             {"ultra", "Ultra (more traffic and people)"}});
            restart_note();
            ch |= ImGui::SliderFloat("Draw distance", &s.draw_distance, 0.5f, 4.0f, "%.2fx");
            restart_note();
            ch |= ImGui::Checkbox("Mipmaps", &s.mipmaps);
            restart_note();
            ch |= combo_int("Anisotropic filtering", &s.anisotropy, {{1, "Off"}, {2, "2x"}, {4, "4x"}, {8, "8x"}, {16, "16x"}});
            ImGui::TextDisabled("Applies to textures as they load.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            ch |= combo_str("Touch controls", &s.touch_controls,
                            {{"auto", "Auto (hide while a controller or keyboard plays)"}, {"show", "Always show"},
                             {"hide", "Always hide"}});
            ch |= ImGui::SliderFloat("Camera speed", &s.mouse_sensitivity, 0.1f, 5.0f, "%.2f");
            ch |= ImGui::Checkbox("Invert camera Y", &s.invert_camera_y);
            ch |= ImGui::SliderFloat("Stick deadzone", &s.stick_deadzone, 0.0f, 0.6f, "%.2f");
            ImGui::Separator();
            if (ImGui::BeginTable("binds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Action");
                ImGui::TableSetupColumn("Controller");
                ImGui::TableSetupColumn("Keyboard / mouse");
                ImGui::TableHeadersRow();
                static const char* rows[][3] = {
                    {"Move / steer", "Left stick", "WASD / arrows"},
                    {"Camera", "Right stick", "Right mouse drag"},
                    {"Attack", "RT (B in a car)", "Space / Left Ctrl"},
                    {"Accelerate / brake", "RT / LT", "W / S"},
                    {"Sprint", "A", "Shift"},
                    {"Cover", "LT / B", "Q"},
                    {"Enter / exit car", "Y (hold to enter when near)", "F"},
                    {"Interact (shop, mission, save)", "X", "E"},
                    {"Weapon prev / next", "LB / RB", "1 / 2, mouse wheel"},
                    {"Map", "Back", "M"},
                    {"Radio on/off, next station", "D-pad up / right", "R / T"},
                    {"Pause / back", "Start (B in menus)", "Esc"},
                    {"Menus", "Stick moves a cursor, A clicks", "Mouse"},
                    {"This menu", "Guide or L3+R3", "F1"},
                };
                for (const auto& r : rows) {
                    ImGui::TableNextRow();
                    for (int i = 0; i < 3; ++i) { ImGui::TableSetColumnIndex(i); ImGui::TextUnformatted(r[i]); }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game")) {
            ch |= ImGui::Checkbox("Pause when the window loses focus", &s.pause_when_unfocused);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Close")) g_open = false;
    ImGui::SameLine();
    ImGui::TextDisabled("Changes are saved to settings.ini");
    ImGui::End();
    return ch;
}

}  // namespace

bool init(SDL_Window* win, SDL_GLContext ctx) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // our own settings file holds everything
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();

    // Size the UI for the screen: ~18 px text at 720p.
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(win), &dm) == 0) h = std::max(h, dm.h * 2 / 3);
    const float px = std::clamp(std::round(18.0f * h / 720.0f), 14.0f, 40.0f);
    ImFont* font = nullptr;
    for (const char* f : {"C:/Windows/Fonts/segoeui.ttf", "/system/fonts/Roboto-Regular.ttf",
                          "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
        std::error_code ec;
        if (std::filesystem::exists(f, ec)) { font = io.Fonts->AddFontFromFileTTF(f, px); break; }
    }
    if (!font) {
        ImFontConfig fc;
        fc.SizePixels = px;
        io.Fonts->AddFontDefault(&fc);
    }
    ImGui::GetStyle().ScaleAllSizes(px / 13.0f);
    ImGui::GetStyle().WindowRounding = 6;
    ImGui::GetStyle().FrameRounding = 4;

    (void)ctx;
    io.BackendPlatformName = "gangstar_sdl2";
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    g_last_frame = std::chrono::steady_clock::now();
    if (!ImGui_ImplOpenGL3_Init("#version 130")) {
        LOG("overlay: ImGui init failed");
        return false;
    }
    g_win = win;
    g_ready = true;
    LOG("overlay: ready (font %.0f px)", px);
    return true;
}

void shutdown() {
    if (!g_ready) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    g_ready = false;
}

bool is_open() { return g_open; }
void set_open(bool open) { g_open = open; }

bool handle_event(const SDL_Event& ev) {
    if (!g_ready) return false;
    // Toggle: F1, controller Guide, or L3+R3 together.
    if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.sym == SDLK_F1) {
        g_open = !g_open;
        return true;
    }
    if (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP) {
        const bool down = ev.type == SDL_CONTROLLERBUTTONDOWN;
        if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK) g_l3 = down;
        if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) g_r3 = down;
        if (down && (ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE ||
                     ((ev.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK ||
                       ev.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) && g_l3 && g_r3))) {
            g_open = !g_open;
            return true;
        }
        // Escape hatch for controllers: B / Start close the menu.
        if (g_open && down && ev.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
            g_open = false;
            return true;
        }
    }
    if (g_open && ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
        g_open = false;
        return true;
    }
    if (!g_open) return false;
    platform_event(ev);
    switch (ev.type) {  // input goes to the overlay only; window/system events still reach the game
        case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT: case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL: case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
        case SDL_CONTROLLERAXISMOTION: case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION:
            return true;
        default:
            return false;
    }
}

void toast(const std::string& msg) {
    g_toasts.push_back({msg, std::chrono::steady_clock::now() + std::chrono::milliseconds(2500)});
    LOG("overlay: %s", msg.c_str());
}

bool draw(settings::Settings& cfg, const FrameStats& st) {
    if (!g_ready) return false;
    const auto now = std::chrono::steady_clock::now();
    g_toasts.erase(std::remove_if(g_toasts.begin(), g_toasts.end(), [&](const Toast& t) { return t.until < now; }),
                   g_toasts.end());
    if (!g_open && !cfg.show_fps && g_toasts.empty()) return false;

    ImGui_ImplOpenGL3_NewFrame();
    platform_new_frame();
    ImGui::NewFrame();
    ImGui::GetIO().MouseDrawCursor = false;
    bool changed = false;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags passive = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (cfg.show_fps) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 8, vp->WorkPos.y + 8), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##fps", nullptr, passive);
        ImGui::Text("%.0f fps  %.1f ms", st.fps, st.fps > 0 ? 1000.0 / st.fps : 0.0);
        ImGui::End();
    }
    if (!g_toasts.empty()) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - 24),
                                ImGuiCond_Always, ImVec2(0.5f, 1));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##toast", nullptr, passive);
        for (const Toast& t : g_toasts) ImGui::TextUnformatted(t.text.c_str());
        ImGui::End();
    }
    if (g_open) changed = settings_window(cfg, st);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return changed;
}

}  // namespace ui
