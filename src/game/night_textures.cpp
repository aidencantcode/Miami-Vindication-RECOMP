#include "game/night_textures.h"

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <utility>
#include <cstring>
#include <deque>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "gl/gles1.h"

namespace nighttex {

namespace {

namespace fs = std::filesystem;

constexpr int kFingerprintBytes = 1024;  // of a mip level's compressed data
constexpr int kSwapsPerFrame = 4;        // texture re-uploads per frame while switching

// Day textures of the static world that have no night twin in the data (ground, grass, concrete,
// props, water, the distant-city LOD). At night they get the darkening the night atlases show:
// a least-squares fit over all 66 atlas pairs (lit windows excluded) gives night = day * k.
constexpr float kNightTint[3] = {0.201f, 0.211f, 0.321f};
const char* const kTintNames[] = {
    "concrete_00", "concrete_01", "concrete_02", "concretepost", "entrance", "etile", "fence00", "fence02",
    "fields", "fields_cap", "forest", "ground_00", "ground_01", "helipad", "jump_00", "lod_city_01",
    "lod_city_02", "lod_city_03", "lod_city_04", "MetroMover_00", "metro_mover_00", "MetroStop_Roof", "park_01",
    "parking_lot_blank", "planecrash01", "prop_gun_col", "props_alpha_02", "bridge_graffitti",
    "grafitti_tags_01_00", "sm_pool_inside", "sm_pool_waterrelf", "stairs_01", "swamp_sentier_00_00",
    "swamp_sentier_01_00", "swamp_water", "tennis", "tile", "water", "water_lo", "water_boat_tex", "sea_foam_00",
    "alphablend_anim_foutain_00"};

struct Pair { fs::path day, night; bool tint = false; };  // tint: no night file, darken the day image

// Scales every colour of DXT data by k. Colours inside a block are interpolated linearly between
// its two RGB565 endpoints, so scaling the endpoints scales all of them; no decode needed. DXT1
// picks its mode from the endpoint order (c0 > c1: 4 colours; else 3 + transparent), so a pair
// that changes order is swapped back and its indices remapped.
void tint_dxt(uint8_t* data, int size, int block, const float* k) {
    auto scale = [&](uint16_t v) {
        const int r = std::min(31, int(std::lround(((v >> 11) & 31) * k[0])));
        const int g = std::min(63, int(std::lround(((v >> 5) & 63) * k[1])));
        const int b = std::min(31, int(std::lround((v & 31) * k[2])));
        return uint16_t((r << 11) | (g << 5) | b);
    };
    for (int off = 0; off + block <= size; off += block) {
        uint8_t* c = data + off + (block == 16 ? 8 : 0);  // DXT3/5: colour block after the alpha block
        const uint16_t c0 = uint16_t(c[0] | c[1] << 8), c1 = uint16_t(c[2] | c[3] << 8);
        uint32_t idx = uint32_t(c[4]) | uint32_t(c[5]) << 8 | uint32_t(c[6]) << 16 | uint32_t(c[7]) << 24;
        uint16_t n0 = scale(c0), n1 = scale(c1);
        if (block == 8) {  // DXT3/5 colour blocks are always 4-colour: order doesn't matter there
            if (c0 > c1) {          // 4-colour mode must stay n0 > n1
                if (n0 == n1) idx = 0;  // one colour; index 0 never means transparent
                else if (n0 < n1) { std::swap(n0, n1); idx ^= 0x55555555u; }  // 0<->1, 2<->3
            } else if (n0 > n1) {   // 3-colour + transparent mode must stay n0 <= n1
                std::swap(n0, n1);
                for (int p = 0; p < 16; ++p)  // 0<->1, keep 2 (midpoint) and 3 (transparent)
                    if (((idx >> (2 * p)) & 3) < 2) idx ^= 1u << (2 * p);
            }
        }
        c[0] = uint8_t(n0); c[1] = uint8_t(n0 >> 8); c[2] = uint8_t(n1); c[3] = uint8_t(n1 >> 8);
        c[4] = uint8_t(idx); c[5] = uint8_t(idx >> 8); c[6] = uint8_t(idx >> 16); c[7] = uint8_t(idx >> 24);
    }
}

struct Dds {
    int w = 0, h = 0, levels = 0, block = 0;  // block = bytes per 4x4 block (8 DXT1, 16 DXT3/5)
    std::vector<uint8_t> bytes;               // whole file (empty when only the header was read)
    int level_offset(int level) const {
        int off = 128;
        for (int i = 0; i < level; ++i) off += level_size(i);
        return off;
    }
    int level_w(int l) const { return std::max(1, w >> l); }
    int level_h(int l) const { return std::max(1, h >> l); }
    int level_size(int l) const { return ((level_w(l) + 3) / 4) * ((level_h(l) + 3) / 4) * block; }
};

// Reads the DDS header (and the whole file when `full`); empty optional if it isn't a DXT DDS.
std::optional<Dds> read_dds(const fs::path& p, bool full) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    uint8_t hdr[128];
    if (!f.read(reinterpret_cast<char*>(hdr), 128) || std::memcmp(hdr, "DDS ", 4) != 0) return std::nullopt;
    auto u32 = [&](int o) { uint32_t v; std::memcpy(&v, hdr + o, 4); return v; };
    Dds d;
    d.h = int(u32(12));
    d.w = int(u32(16));
    d.levels = std::max(1, int(u32(28)));
    const uint32_t fourcc = u32(84);
    if (fourcc == 0x31545844) d.block = 8;  // "DXT1"
    else if (fourcc == 0x33545844 || fourcc == 0x35545844) d.block = 16;  // "DXT3" / "DXT5"
    else return std::nullopt;
    if (full) {
        f.seekg(0, std::ios::end);
        d.bytes.resize(size_t(f.tellg()));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(d.bytes.data()), std::streamsize(d.bytes.size()));
        if (int(d.bytes.size()) < d.level_offset(d.levels)) return std::nullopt;
    }
    return d;
}

uint64_t fingerprint(const uint8_t* data, int size, int w, int h) {
    uint64_t x = 1469598103934665603ull ^ (uint64_t(w) << 32) ^ uint64_t(h);
    for (int i = 0; i < std::min(size, kFingerprintBytes); ++i) x = (x ^ data[i]) * 1099511628211ull;
    return x;
}

struct Known { int pair; int base_level; };  // which pair, which file mip is the texture's level 0
struct Tex {
    int pair = -1, base_level = 0, levels = 0;
    unsigned format = 0;
    bool night = false;  // what it currently shows
};

std::vector<Pair> g_pairs;
std::unordered_map<uint64_t, Known> g_by_fp;
std::unordered_map<unsigned, Tex> g_tex;
std::deque<unsigned> g_queue;
bool g_night = false, g_ready = false;
uint64_t g_swaps = 0;

void scan(const fs::path& dir) {
    const std::regex numbered(R"((alpha_)?night(\d+)\.tga)", std::regex::icase);
    const std::regex suffixed(R"((.+)_night\.tga)", std::regex::icase);
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        std::smatch m;
        fs::path day;
        if (std::regex_match(n, m, numbered)) day = dir / (std::string(m[1]) + "atlas" + std::string(m[2]) + ".tga");
        else if (std::regex_match(n, m, suffixed)) day = dir / (std::string(m[1]) + ".tga");
        else continue;
        // The car reflection map is swapped by the game itself (DayTime::stateChange).
        if (n.rfind("car_body_reflection", 0) == 0 || !fs::exists(day, ec)) continue;
        g_pairs.push_back({day, e.path(), false});
    }
    for (const char* n : kTintNames) {
        const fs::path day = dir / (std::string(n) + ".tga");
        if (fs::exists(day, ec)) g_pairs.push_back({day, {}, true});
    }
}

// Fingerprints of the day file's first three mip levels (the game may skip top levels).
void index_pair(int i) {
    const auto hdr = read_dds(g_pairs[size_t(i)].day, false);
    if (!hdr) return;
    if (!g_pairs[size_t(i)].tint) {
        const auto night = read_dds(g_pairs[size_t(i)].night, false);
        if (!night || night->w != hdr->w || night->h != hdr->h || night->block != hdr->block) return;
    }
    std::ifstream f(g_pairs[size_t(i)].day, std::ios::binary);
    for (int l = 0; l < std::min(3, hdr->levels); ++l) {
        const int size = std::min(hdr->level_size(l), kFingerprintBytes);
        std::vector<uint8_t> buf(size_t(size), 0);
        f.seekg(hdr->level_offset(l));
        if (!f.read(reinterpret_cast<char*>(buf.data()), size)) break;
        g_by_fp[fingerprint(buf.data(), hdr->level_size(l), hdr->level_w(l), hdr->level_h(l))] = {i, l};
    }
}

void on_upload(unsigned tex, int level, unsigned format, int w, int h, const uint8_t* data, int size) {
    if (!tex) return;
    if (level == 0) {
        auto it = g_by_fp.find(fingerprint(data, size, w, h));
        if (it == g_by_fp.end()) {
            if (g_tex.erase(tex)) gles1::set_world_texture(tex, gles1::WorldTex::None);  // the id now holds something else
            return;
        }
        Tex& t = g_tex[tex];
        t = {it->second.pair, it->second.base_level, 1, format, false};
        gles1::set_world_texture(tex, gles1::WorldTex::Day);  // shows its day image: gets the time-of-day tint
        if (g_night) g_queue.push_back(tex);  // loaded during the night: give it its night image
    } else if (auto it = g_tex.find(tex); it != g_tex.end()) {
        it->second.levels = std::max(it->second.levels, level + 1);
    }
}

void swap(unsigned tex, bool night) {
    auto it = g_tex.find(tex);
    if (it == g_tex.end() || it->second.night == night) return;
    Tex& t = it->second;
    const Pair& p = g_pairs[size_t(t.pair)];
    const fs::path& file = night && !p.tint ? p.night : p.day;
    auto dds = read_dds(file, true);
    if (!dds || t.base_level + t.levels > dds->levels) {
        LOG("night textures: cannot use %s", file.filename().string().c_str());
        gles1::set_world_texture(tex, gles1::WorldTex::None);
        g_tex.erase(it);
        return;
    }
    if (night && p.tint)
        tint_dxt(dds->bytes.data() + 128, int(dds->bytes.size()) - 128, dds->block, kNightTint);
    std::vector<gles1::CompressedLevel> levels;
    for (int l = 0; l < t.levels; ++l) {
        const int fl = t.base_level + l;
        levels.push_back({dds->level_w(fl), dds->level_h(fl), dds->bytes.data() + dds->level_offset(fl),
                          dds->level_size(fl)});
    }
    gles1::replace_compressed_levels(tex, t.format, levels);
    t.night = night;
    gles1::set_world_texture(tex, night ? gles1::WorldTex::Night : gles1::WorldTex::Day);  // night images are final
    ++g_swaps;
}

}  // namespace

bool init(const fs::path& data_dir) {
    scan(data_dir);
    for (int i = 0; i < int(g_pairs.size()); ++i) index_pair(i);
    gles1::TextureObserver obs;
    obs.on_compressed_upload = on_upload;
    obs.on_delete = [](unsigned tex) {
        g_tex.erase(tex);
        gles1::set_world_texture(tex, gles1::WorldTex::None);
    };
    gles1::set_texture_observer(std::move(obs));
    g_ready = !g_by_fp.empty();
    LOG("night textures: %zu day/night pairs in the data, %zu fingerprints", g_pairs.size(), g_by_fp.size());
    return g_ready;
}

// Time-of-day tint for the world while it shows day images (minutes of the game day). Night
// (DayTime state 3) is kNightTint; dusk 15:00-18:59 and dawn 03:00-07:00 blend through golden,
// orange and purple keyframes into and out of it, so the city follows the sky instead of staying in
// noon light until it snaps to night. Matched to the game's sky colours by screenshots.
struct Key { int minute; float r, g, b; };
const Key kKeys[] = {
    {180, kNightTint[0], kNightTint[1], kNightTint[2]},  // 03:00 dawn starts (night images go away)
    {240, 0.30f, 0.29f, 0.40f},
    {300, 0.62f, 0.50f, 0.56f},  // 05:00 pink first light
    {345, 0.90f, 0.74f, 0.66f},
    {390, 1.00f, 0.93f, 0.86f},
    {420, 1.00f, 1.00f, 1.00f},  // 07:00 day
    {900, 1.00f, 1.00f, 1.00f},  // 15:00 dusk starts
    {1020, 1.00f, 0.93f, 0.80f}, // golden hour
    {1080, 0.86f, 0.66f, 0.55f}, // 18:00 sunset orange
    {1110, 0.55f, 0.42f, 0.50f}, // purple
    {1139, kNightTint[0], kNightTint[1], kNightTint[2]},  // 18:59 night (images swap in)
};
void tint_at(int minute, float* c) {
    const int n = int(sizeof kKeys / sizeof kKeys[0]);
    if (minute < kKeys[0].minute || minute >= kKeys[n - 1].minute) { std::copy(kNightTint, kNightTint + 3, c); return; }
    for (int i = 0; i + 1 < n; ++i) {
        const Key& a = kKeys[i];
        const Key& b = kKeys[i + 1];
        if (minute >= a.minute && minute < b.minute) {
            const float t = float(minute - a.minute) / float(b.minute - a.minute);
            c[0] = a.r + (b.r - a.r) * t;
            c[1] = a.g + (b.g - a.g) * t;
            c[2] = a.b + (b.b - a.b) * t;
            return;
        }
    }
}

void update(bool enabled, int state, int minute, float people_lift) {
    if (!g_ready) return;
    const bool night = enabled && state == 3;
    float c[3] = {1, 1, 1};
    if (night) std::copy(kNightTint, kNightTint + 3, c);
    else if (enabled) tint_at(((minute % 1440) + 1440) % 1440, c);
    gles1::set_world_tint(c[0], c[1], c[2]);
    // People and cars: the same colour, lifted toward 1 so they stay readable (their vertex colours
    // already carry the game's own night light).
    const float lift = std::clamp(people_lift, 0.0f, 1.0f);
    gles1::set_actor_tint(c[0] + (1 - c[0]) * lift, c[1] + (1 - c[1]) * lift, c[2] + (1 - c[2]) * lift);
    if (night != g_night) {
        g_night = night;
        g_queue.clear();
        for (const auto& [tex, t] : g_tex)
            if (t.night != night) g_queue.push_back(tex);
        LOG("night textures: switching %zu textures to %s", g_queue.size(), night ? "night" : "day");
    }
    for (int i = 0; i < kSwapsPerFrame && !g_queue.empty(); ++i) {
        const unsigned tex = g_queue.front();
        g_queue.pop_front();
        swap(tex, g_night);
    }
}

}  // namespace nighttex
