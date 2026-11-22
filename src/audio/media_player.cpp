#include "audio/media_player.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "audio/mixer.h"
#include "core/log.h"

namespace media {

namespace {

constexpr int kPoolVoices = 16;    // SoundPool(5, ...) on the phone
constexpr int kMaxStreams = 20;    // Java releases non-music players above this
constexpr float kBigDivisor = 40;  // GLMediaPlayer.dx on non-Motorola-Droid phones

Config g_cfg;
std::vector<std::string> g_java;                  // Java names (index = sound id)
std::vector<std::optional<std::string>> g_path;   // resolved host path ("" = missing)
float g_music_gain = 1, g_sfx_gain = 1, g_vfx_gain = 1;

struct Pool { std::shared_ptr<const audio::Clip> clip; bool tried = false; audio::VoiceId voice = 0; };
std::unordered_map<int, Pool> g_pool;
std::deque<int> g_pool_order;  // sound ids in the order they started (for the voice cap)

struct Big { audio::VoiceId voice = 0; bool paused_by_all = false; };
std::unordered_map<int, Big> g_big;
std::vector<int> g_active;  // like Java's 'dc': streams started and not stopped

uint64_t g_plays = 0, g_big_plays = 0, g_restored = 0;
std::vector<int> g_missing;

const std::string& java_name(int i) {
    static const std::string none;
    return i >= 0 && i < int(g_java.size()) ? g_java[i] : none;
}

// GANGSTAR_AUDIO_TRACE=1: log every call the game makes (seconds since start, call, id, name).
bool tracing() {
    static const bool on = [] { const char* e = std::getenv("GANGSTAR_AUDIO_TRACE"); return e && *e && *e != '0'; }();
    return on;
}
void trace(const char* what, int i, float a = 0, float b = 0) {
    if (!tracing()) return;
    static const auto t0 = std::chrono::steady_clock::now();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LOG("audio-trace %9.3f %-14s %4d %-28s %.3f %.3f", t, what, i, java_name(i).c_str(), a, b);
}

audio::Group group_of(int i) {
    const std::string& n = java_name(i);
    if (n.rfind("m_", 0) == 0) return audio::kMusic;
    if (n.rfind("vfx_", 0) == 0 || n.rfind("cin_", 0) == 0) return audio::kVoice;
    return audio::kSfx;
}

// Host path of sound i: Java's name, else (optionally) the game's own name with .ogg.
const std::string& path_of(int i) {
    static const std::string none;
    if (i < 0) return none;
    if (i >= int(g_path.size())) g_path.resize(i + 1);
    if (g_path[i]) return *g_path[i];
    std::error_code ec;
    std::string found;
    if (!java_name(i).empty() && std::filesystem::exists(g_cfg.data_dir / java_name(i), ec))
        found = (g_cfg.data_dir / java_name(i)).string();
    if (found.empty() && g_cfg.restore_missing && g_cfg.native_name) {
        std::string n = g_cfg.native_name(i);
        const size_t dot = n.rfind('.');
        if (!n.empty()) {
            n = (dot == std::string::npos ? n : n.substr(0, dot)) + ".ogg";
            if (std::filesystem::exists(g_cfg.data_dir / n, ec)) {
                found = (g_cfg.data_dir / n).string();
                ++g_restored;
                LOG("audio: sound %d '%s' missing; using the game's own name '%s'", i, java_name(i).c_str(), n.c_str());
            }
        }
    }
    if (found.empty()) {
        g_missing.push_back(i);
        LOG("audio: sound %d '%s' not found in the data", i, java_name(i).c_str());
    }
    g_path[i] = found;
    return *g_path[i];
}

void enforce_pool_cap() {
    int alive = 0;
    for (int id : g_pool_order)
        if (audio::alive(g_pool[id].voice)) ++alive;
    while (alive > kPoolVoices && !g_pool_order.empty()) {
        const int id = g_pool_order.front();
        g_pool_order.pop_front();
        if (audio::alive(g_pool[id].voice)) {
            audio::stop(g_pool[id].voice);
            --alive;
        }
    }
    // forget finished entries
    g_pool_order.erase(std::remove_if(g_pool_order.begin(), g_pool_order.end(),
                                      [](int id) { return !audio::alive(g_pool[id].voice); }),
                       g_pool_order.end());
}

}  // namespace

bool init(const Config& cfg) {
    g_cfg = cfg;
    std::ifstream f(cfg.table_file);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        g_java.push_back(line);
    }
    if (g_java.empty())
        LOG("audio: sound table %s missing or empty (run tools/extract_sound_table.py); using the game's own names",
            cfg.table_file.string().c_str());
    else
        LOG("audio: %zu sound names from %s", g_java.size(), cfg.table_file.string().c_str());
    if (g_java.empty()) g_cfg.restore_missing = true;
    return audio::init();
}

// ---- SoundPool ------------------------------------------------------------------------------------
void load_sound(int i) {
    Pool& p = g_pool[i];
    if (p.tried) return;
    p.tried = true;
    const std::string& path = path_of(i);
    if (!path.empty()) p.clip = audio::load_clip(path);
}

void unload_sound(int i) {
    auto it = g_pool.find(i);
    if (it == g_pool.end()) return;
    audio::stop(it->second.voice);
    g_pool.erase(it);
}

int is_sound_loaded(int i) {
    auto it = g_pool.find(i);
    return it != g_pool.end() && it->second.clip ? 0 : -1;
}

void play_sound(int i, float volume, float rate) {
    trace("play", i, volume, rate);
    load_sound(i);
    Pool& p = g_pool[i];
    if (!p.clip) return;
    if (p.voice) audio::stop(p.voice);  // one stream per sound id, like Java's da[i]
    const float vol = std::clamp(volume * g_sfx_gain, 0.0f, 1.0f);
    p.voice = audio::play_clip(p.clip, vol, std::clamp(rate, 0.5f, 2.0f), false, group_of(i));
    g_pool_order.push_back(i);
    enforce_pool_cap();
    ++g_plays;
}

void stop_sound(int i) {
    trace("stop", i);
    auto it = g_pool.find(i);
    if (it == g_pool.end() || !it->second.voice) return;
    audio::stop(it->second.voice);
    it->second.voice = 0;
}

void pause_sound(int i) { if (auto it = g_pool.find(i); it != g_pool.end()) audio::set_paused(it->second.voice, true); }
void resume_sound(int i) { if (auto it = g_pool.find(i); it != g_pool.end()) audio::set_paused(it->second.voice, false); }
void set_pitch(int i, float rate) {
    if (auto it = g_pool.find(i); it != g_pool.end()) audio::set_rate(it->second.voice, std::clamp(rate, 0.5f, 2.0f));
}

void stop_all_pool(int except) {
    for (auto& [id, p] : g_pool)
        if (id != except && p.voice) {
            audio::stop(p.voice);
            p.voice = 0;
        }
}

void reset_pool() {
    stop_all_pool(-1);
    g_pool.clear();
    g_pool_order.clear();
}

// ---- MediaPlayer ----------------------------------------------------------------------------------
void load_sound_big(int i) {
    trace("load_big", i);
    Big& b = g_big[i];
    if (b.voice && audio::alive(b.voice)) return;
    const std::string& path = path_of(i);
    if (!path.empty()) b.voice = audio::open_stream(path, 0, false, group_of(i));
}

void unload_sound_big(int i) { stop_sound_big(i); }

int is_sound_loaded_big(int i) {
    auto it = g_big.find(i);
    return it != g_big.end() && it->second.voice ? 0 : -1;
}

void play_sound_big(int i, float volume, int loop) {
    trace("play_big", i, volume, float(loop));
    const std::string& n = java_name(i);
    const float gain = n.rfind("m_", 0) == 0 ? g_music_gain : n.rfind("sfx_", 0) == 0 ? g_sfx_gain
                     : (n.rfind("vfx_", 0) == 0 || n.rfind("cin_", 0) == 0) ? g_vfx_gain : (n.empty() ? g_sfx_gain : 0.0f);
    if (int(g_active.size()) > kMaxStreams) {  // Java: release up to 20 non-music players
        for (int id : std::vector<int>(g_active))
            if (java_name(id).rfind("m_", 0) != 0 && !audio::playing(g_big[id].voice)) stop_sound_big(id);
    }
    Big& b = g_big[i];
    if (!b.voice || !audio::alive(b.voice)) {
        // never loaded, stopped, or finished (MediaPlayer.start() after completion restarts)
        b.voice = 0;
        load_sound_big(i);
        if (!b.voice) return;
    } else if (i >= 207 && i <= 259) {
        audio::rewind(b.voice);  // as Java: these restart from the beginning
    }
    audio::set_volume(b.voice, std::clamp(gain * volume / kBigDivisor, 0.0f, 1.0f));
    audio::set_loop(b.voice, loop == 1);
    audio::set_paused(b.voice, false);
    b.paused_by_all = false;
    if (std::find(g_active.begin(), g_active.end(), i) == g_active.end()) g_active.push_back(i);
    ++g_big_plays;
}

void stop_sound_big(int i) {
    trace("stop_big", i);
    auto it = g_big.find(i);
    if (it == g_big.end()) return;
    audio::stop(it->second.voice);
    g_big.erase(it);
    g_active.erase(std::remove(g_active.begin(), g_active.end(), i), g_active.end());
}

void pause_sound_big(int i) {
    trace("pause_big", i);
    if (auto it = g_big.find(i); it != g_big.end() && audio::playing(it->second.voice)) audio::set_paused(it->second.voice, true);
}

void resume_sound_big(int i) {
    trace("resume_big", i);
    if (auto it = g_big.find(i); it != g_big.end() && audio::paused(it->second.voice)) audio::set_paused(it->second.voice, false);
}

void set_volume_big(int i, float volume) {
    if (auto it = g_big.find(i); it != g_big.end())
        audio::set_volume(it->second.voice, std::clamp(volume / kBigDivisor, 0.0f, 1.0f));
}

void reset_sound(int i) { trace("reset", i); if (auto it = g_big.find(i); it != g_big.end()) audio::rewind(it->second.voice); }

int is_media_playing(int i) {
    auto it = g_big.find(i);
    const int r = it != g_big.end() && audio::playing(it->second.voice) ? 1 : 0;
    if (!r && it != g_big.end()) trace(!audio::alive(it->second.voice) ? "playing? no (ended)" : "playing? no (paused)", i);
    return r;
}

void pause_all_sounds() {
    trace("pause_all", -1);
    for (int id : g_active) {
        Big& b = g_big[id];
        if (audio::playing(b.voice)) {
            audio::set_paused(b.voice, true);
            b.paused_by_all = true;
        }
    }
}

void resume_all_sounds() {
    trace("resume_all", -1);
    for (int id : g_active) {
        Big& b = g_big[id];
        if (b.paused_by_all) {
            audio::set_paused(b.voice, false);
            b.paused_by_all = false;
        }
    }
}

void stop_all_big() {
    trace("stop_all_big", -1);
    for (int id : std::vector<int>(g_active)) stop_sound_big(id);
}

void set_music_gain(float g) { g_music_gain = g; LOG("audio: music gain %.3f", g); }
void set_sfx_gain(float g) { g_sfx_gain = g; LOG("audio: sfx gain %.3f", g); }
void set_vfx_gain(float g) { g_vfx_gain = g; LOG("audio: voice gain %.3f", g); }

void report() {
    LOG("audio: %llu effect plays, %llu stream plays, %llu sounds found by the game's own name, %zu missing",
        (unsigned long long)g_plays, (unsigned long long)g_big_plays, (unsigned long long)g_restored, g_missing.size());
}

}  // namespace media
