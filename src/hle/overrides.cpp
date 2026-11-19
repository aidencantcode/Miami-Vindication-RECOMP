#include "hle/overrides.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>

#include "core/log.h"

namespace hle::overrides {

namespace fs = std::filesystem;

namespace {
fs::path g_dir;
std::map<std::string, std::vector<Transform>> g_transforms;  // lower-case file name -> transforms
std::map<std::string, fs::path> g_done;                       // lower-case file name -> override copy

std::string lower(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}
}  // namespace

void set_dir(const fs::path& dir) { g_dir = dir; }

void add(const std::string& file_name, Transform t) { g_transforms[lower(file_name)].push_back(std::move(t)); }

fs::path redirect(const fs::path& host) {
    const std::string name = lower(host.filename().string());
    auto it = g_transforms.find(name);
    if (it == g_transforms.end() || g_dir.empty()) return host;
    if (auto d = g_done.find(name); d != g_done.end()) return d->second;
    std::ifstream in(host, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
    bool changed = false;
    for (const Transform& t : it->second) changed |= t(data);
    fs::path out = host;
    if (changed) {
        std::error_code ec;
        fs::create_directories(g_dir, ec);
        out = g_dir / host.filename();
        std::ofstream o(out, std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!o) {
            LOG("overrides: cannot write %s, using the original", out.string().c_str());
            out = host;
        } else {
            LOG("overrides: %s -> %s", host.filename().string().c_str(), out.string().c_str());
        }
    }
    g_done[name] = out;
    return out;
}

bool string_table_set(std::vector<uint8_t>& d, int index, const std::string& text) {
    if (d.size() < 2) return false;
    const int count = d[0] | (d[1] << 8);
    if (index < 0 || index >= count) return false;
    size_t o = 2;
    for (int i = 0; i < count; ++i) {
        if (o + 2 > d.size()) return false;
        const size_t len = d[o] | (d[o + 1] << 8);
        if (o + 2 + len > d.size()) return false;
        if (i == index) {
            if (text.size() > 0xffff) return false;
            std::vector<uint8_t> repl = {uint8_t(text.size() & 0xff), uint8_t(text.size() >> 8)};
            repl.insert(repl.end(), text.begin(), text.end());
            d.erase(d.begin() + std::ptrdiff_t(o), d.begin() + std::ptrdiff_t(o + 2 + len));
            d.insert(d.begin() + std::ptrdiff_t(o), repl.begin(), repl.end());
            return true;
        }
        o += 2 + len;
    }
    return false;
}

}  // namespace hle::overrides
