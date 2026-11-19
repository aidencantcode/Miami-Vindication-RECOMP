#include "settings/settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <vector>

#include "core/log.h"

namespace settings {

namespace {

// One table describes every key: section, name, and how to read/write it.
struct Field {
    const char* section;
    const char* key;
    std::function<void(Settings&, const std::string&)> read;
    std::function<std::string(const Settings&)> write;
};

template <class T> Field F(const char* sec, const char* key, T Settings::*m);

template <> Field F<std::string>(const char* sec, const char* key, std::string Settings::*m) {
    return {sec, key, [m](Settings& s, const std::string& v) { s.*m = v; }, [m](const Settings& s) { return s.*m; }};
}
template <> Field F<int>(const char* sec, const char* key, int Settings::*m) {
    return {sec, key, [m](Settings& s, const std::string& v) { s.*m = std::atoi(v.c_str()); },
            [m](const Settings& s) { return std::to_string(s.*m); }};
}
template <> Field F<float>(const char* sec, const char* key, float Settings::*m) {
    return {sec, key, [m](Settings& s, const std::string& v) { s.*m = float(std::atof(v.c_str())); },
            [m](const Settings& s) { char b[32]; std::snprintf(b, sizeof b, "%g", s.*m); return std::string(b); }};
}
template <> Field F<bool>(const char* sec, const char* key, bool Settings::*m) {
    return {sec, key, [m](Settings& s, const std::string& v) { s.*m = v == "1" || v == "true" || v == "yes" || v == "on"; },
            [m](const Settings& s) { return std::string(s.*m ? "true" : "false"); }};
}

const std::vector<Field>& fields() {
    static const std::vector<Field> f = {
        F("display", "window_mode", &Settings::window_mode),
        F("display", "window_width", &Settings::window_w),
        F("display", "window_height", &Settings::window_h),
        F("display", "vsync", &Settings::vsync),
        F("display", "fps_limit", &Settings::fps_limit),
        F("display", "aspect", &Settings::aspect),
        F("display", "render_scale", &Settings::render_scale),
        F("display", "msaa", &Settings::msaa),
        F("display", "fit", &Settings::fit),
        F("display", "smooth_scaling", &Settings::smooth_scaling),
        F("display", "show_fps", &Settings::show_fps),
        F("display", "low_latency", &Settings::low_latency),
        F("graphics", "quality", &Settings::quality),
        F("graphics", "mipmaps", &Settings::mipmaps),
        F("graphics", "draw_distance", &Settings::draw_distance),
        F("graphics", "anisotropy", &Settings::anisotropy),
        F("graphics", "texture_filter", &Settings::texture_filter),
        F("controls", "touch_controls", &Settings::touch_controls),
        F("controls", "mouse_sensitivity", &Settings::mouse_sensitivity),
        F("controls", "invert_camera_y", &Settings::invert_camera_y),
        F("controls", "mouse_look", &Settings::mouse_look),
        F("controls", "stick_deadzone", &Settings::stick_deadzone),
        F("audio", "master_volume", &Settings::master_volume),
        F("audio", "music_volume", &Settings::music_volume),
        F("audio", "sfx_volume", &Settings::sfx_volume),
        F("audio", "voice_volume", &Settings::voice_volume),
        F("audio", "restore_missing_sounds", &Settings::restore_missing_sounds),
        F("game", "skip_intro", &Settings::skip_intro),
        F("game", "pause_when_unfocused", &Settings::pause_when_unfocused),
        F("game", "hide_store_buttons", &Settings::hide_store_buttons),
        F("game", "custom_text", &Settings::custom_text),
        F("game", "fast_forward_speed", &Settings::fast_forward_speed),
    };
    return f;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

}  // namespace

bool load(const std::filesystem::path& file, Settings* s) {
    std::ifstream in(file);
    if (!in) return false;
    std::string line, section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size() - 2); continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        bool known = false;
        for (const Field& f : fields())
            if (section == f.section && key == f.key) { f.read(*s, val); known = true; }
        if (!known) LOG("settings: ignoring unknown key [%s] %s", section.c_str(), key.c_str());
    }
    return true;
}

bool save(const std::filesystem::path& file, const Settings& s) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file);
    if (!out) return false;
    out << "; Gangstar: Miami Vindication native port - settings (edit freely; the in-game menu is F1)\n";
    std::string section;
    for (const Field& f : fields()) {
        if (section != f.section) { section = f.section; out << "\n[" << section << "]\n"; }
        out << f.key << " = " << f.write(s) << "\n";
    }
    return bool(out);
}

bool apply_override(Settings* s, const std::string& assignment) {
    const size_t dot = assignment.find('.'), eq = assignment.find('=');
    if (dot == std::string::npos || eq == std::string::npos || dot > eq) return false;
    const std::string sec = assignment.substr(0, dot), key = assignment.substr(dot + 1, eq - dot - 1);
    for (const Field& f : fields())
        if (sec == f.section && key == f.key) {
            f.read(*s, assignment.substr(eq + 1));
            return true;
        }
    return false;
}

int logical_width(const Settings& s, int window_w, int window_h) {
    static const std::map<std::string, double> fixed = {{"4:3", 4.0 / 3}, {"5:3", 5.0 / 3}, {"16:10", 1.6},
                                                        {"16:9", 16.0 / 9}, {"21:9", 21.0 / 9}};
    double aspect = window_h > 0 ? double(window_w) / window_h : 5.0 / 3;
    if (auto it = fixed.find(s.aspect); it != fixed.end()) aspect = it->second;
    int w = int(std::lround(480 * aspect / 2.0)) * 2;  // even width
    return std::clamp(w, 640, 1280);  // the game lays out for a 480-high screen of any width (tested 800, 854)
}

int auto_render_scale(int window_h) { return std::clamp(int(std::ceil(window_h / 480.0)), 1, 8); }

}  // namespace settings
