// HLE for formatted I/O (printf/sscanf families), stdio + POSIX files and liblog.
// C++ twin of the matching parts of tools/probe/hle_libc.py.
//
// File system overlay (same rules as the probe):
//   /sdcard/gameloft/games/Gangstar2/...  -> the owner's data folder, READ-ONLY
//   /data/data/<package>/...              -> sandbox/data/...   (saves, preferences)
//   anything else                         -> sandbox/other/...
//   every write goes to sandbox/; a write to an existing data file copies it first.
// Layouts (bionic, 32-bit ARM): FILE 84 bytes (_flags @12, _file @14); struct stat
// 104 bytes (st_mode @16, st_size int64 @48, st_mtime @80); struct dirent: d_ino u64,
// d_off s64, d_reclen u16 @16, d_type u8 @18, d_name @19.
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "hle/hle.h"
#include "rt/runtime.h"

namespace fs = std::filesystem;

namespace hle {

using cpu::Args;
using cpu::Cpu;

// ===================================================================== formatting
namespace {

template <class T>
void append_fmt(std::string& out, const std::string& spec, T v) {
    int n = std::snprintf(nullptr, 0, spec.c_str(), v);
    if (n <= 0) return;
    size_t at = out.size();
    out.resize(at + size_t(n) + 1);
    std::snprintf(out.data() + at, size_t(n) + 1, spec.c_str(), v);
    out.resize(at + size_t(n));
}

std::string utf8_of_wide(Cpu& c, uint32_t p) {
    std::string s;
    auto& m = c.rt().mem();
    for (uint32_t ch; (ch = m.r32(p)) != 0; p += 4) {
        if (ch < 0x80) s += char(ch);
        else if (ch < 0x800) s += char(0xC0 | (ch >> 6)), s += char(0x80 | (ch & 0x3F));
        else if (ch < 0x10000) s += char(0xE0 | (ch >> 12)), s += char(0x80 | ((ch >> 6) & 0x3F)), s += char(0x80 | (ch & 0x3F));
        else s += char(0xF0 | (ch >> 18)), s += char(0x80 | ((ch >> 12) & 0x3F)), s += char(0x80 | ((ch >> 6) & 0x3F)), s += char(0x80 | (ch & 0x3F));
    }
    return s;
}

}  // namespace

std::string cformat(Cpu& c, const char* fmt, Args& args) {
    std::string out;
    for (const char* p = fmt; *p;) {
        if (*p != '%') { out += *p++; continue; }
        const char* start = p++;
        if (*p == '%') { out += '%'; ++p; continue; }
        std::string flags, width, prec;
        bool has_prec = false;
        while (*p && std::strchr("-+ #0", *p)) flags += *p++;
        if (*p == '*') {
            int w = args.s32();
            if (w < 0) flags += '-', w = -w;
            width = std::to_string(w);
            ++p;
        } else {
            while (std::isdigit(uint8_t(*p))) width += *p++;
        }
        if (*p == '.') {
            has_prec = true;
            ++p;
            if (*p == '*') {
                int pr = args.s32();
                ++p;
                if (pr >= 0) prec = std::to_string(pr); else has_prec = false;
            } else {
                while (std::isdigit(uint8_t(*p))) prec += *p++;
                if (prec.empty()) prec = "0";
            }
        }
        enum { NONE, HH, HALF, LONG, LLONG } len = NONE;
        if (p[0] == 'h' && p[1] == 'h') len = HH, p += 2;
        else if (*p == 'h') len = HALF, ++p;
        else if (p[0] == 'l' && p[1] == 'l') len = LLONG, p += 2;
        else if (*p == 'q' || *p == 'j') len = LLONG, ++p;
        else if (*p == 'l') len = LONG, ++p;
        else if (*p == 'L' || *p == 'z' || *p == 't') ++p;  // long double == double; size_t/ptrdiff_t are 32-bit
        const char conv = *p ? *p++ : 0;
        const std::string spec = "%" + flags + width + (has_prec ? "." + prec : "");
        switch (conv) {
            case 'd': case 'i': {
                long long v = len == LLONG ? args.s64() : args.s32();
                if (len == HALF) v = int16_t(v);
                if (len == HH) v = int8_t(v);
                append_fmt(out, spec + "lld", v);
                break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                unsigned long long v = len == LLONG ? args.u64() : args.u32();
                if (len == HALF) v = uint16_t(v);
                if (len == HH) v = uint8_t(v);
                append_fmt(out, spec + "ll" + conv, v);
                break;
            }
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
                append_fmt(out, spec + conv, args.f64());  // floats are promoted to double
                break;
            case 'c': append_fmt(out, "%" + flags + width + "c", int(args.u32() & 0xFF)); break;
            case 's': {
                uint32_t ptr = args.u32();
                std::string text = !ptr ? "(null)" : len == LONG ? utf8_of_wide(c, ptr) : std::string(c.rt().mem().cstr(ptr));
                append_fmt(out, spec + "s", text.c_str());
                break;
            }
            case 'p': {
                char b[16];
                std::snprintf(b, sizeof b, "0x%x", args.u32());
                append_fmt(out, "%" + flags + width + "s", b);
                break;
            }
            case 'n': args.u32(); break;
            default: out.append(start, p); break;
        }
    }
    return out;
}

int csscanf(Cpu& c, const char* s, const char* fmt, Args& args) {
    auto& m = c.rt().mem();
    const char* in = s;
    int assigned = 0;
    bool converted = false;
    auto is_ws = [](char ch) { return ch == ' ' || (ch >= '\t' && ch <= '\r'); };
    for (const char* f = fmt; *f;) {
        if (is_ws(*f)) { while (is_ws(*in)) ++in; ++f; continue; }
        if (*f != '%') { if (*in == *f) { ++in; ++f; continue; } break; }
        ++f;
        if (*f == '%') { while (is_ws(*in)) ++in; if (*in == '%') { ++in; ++f; continue; } break; }
        bool suppress = false;
        if (*f == '*') suppress = true, ++f;
        int width = 0;
        while (std::isdigit(uint8_t(*f))) width = width * 10 + (*f++ - '0');
        int size = 4;
        bool dbl = false;
        if (f[0] == 'h' && f[1] == 'h') size = 1, f += 2;
        else if (*f == 'h') size = 2, ++f;
        else if (f[0] == 'l' && f[1] == 'l') size = 8, f += 2;
        else if (*f == 'l' || *f == 'L') dbl = true, ++f;
        else if (*f == 'q' || *f == 'j') size = 8, ++f;
        else if (*f == 'z' || *f == 't') ++f;
        char conv = *f++;
        if (conv == 'n') { if (!suppress) m.w32(args.u32(), uint32_t(in - s)); continue; }
        if (conv != 'c' && conv != '[') while (is_ws(*in)) ++in;
        if (!*in) return converted ? assigned : -1;
        const size_t avail = std::strlen(in);
        const size_t lim = width ? std::min<size_t>(size_t(width), avail) : avail;
        std::string chunk(in, lim);
        if (std::strchr("diuoxXp", conv)) {
            int base = conv == 'i' ? 0 : conv == 'o' ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : 10;
            char* end;
            long long v = (conv == 'u') ? (long long)std::strtoull(chunk.c_str(), &end, base) : std::strtoll(chunk.c_str(), &end, base);
            if (end == chunk.c_str()) break;
            in += end - chunk.c_str();
            if (!suppress) {
                uint32_t p = args.u32();
                m.write(p, &v, size_t(size));  // little-endian: low bytes first
                ++assigned;
            }
        } else if (std::strchr("eEfFgGaA", conv)) {
            char* end;
            double v = std::strtod(chunk.c_str(), &end);
            if (end == chunk.c_str()) break;
            in += end - chunk.c_str();
            if (!suppress) {
                uint32_t p = args.u32();
                if (dbl) m.write(p, &v, 8);
                else { float fv = float(v); m.write(p, &fv, 4); }
                ++assigned;
            }
        } else if (conv == 's') {
            size_t j = 0;
            while (j < chunk.size() && !is_ws(chunk[j])) ++j;
            if (!j) break;
            if (!suppress) { uint32_t p = args.u32(); m.write(p, chunk.data(), j); m.w8(p + uint32_t(j), 0); ++assigned; }
            in += j;
        } else if (conv == 'c') {
            size_t n = width ? size_t(width) : 1;
            if (avail < n) break;
            if (!suppress) { m.write(args.u32(), in, n); ++assigned; }
            in += n;
        } else if (conv == '[') {
            bool neg = *f == '^';
            if (neg) ++f;
            std::set<uint8_t> set;
            if (*f == ']') set.insert(']'), ++f;
            while (*f && *f != ']') {
                if (f[1] == '-' && f[2] && f[2] != ']') { for (int ch = uint8_t(f[0]); ch <= uint8_t(f[2]); ++ch) set.insert(uint8_t(ch)); f += 3; }
                else set.insert(uint8_t(*f++));
            }
            if (*f == ']') ++f;
            size_t j = 0;
            while (j < chunk.size() && (set.count(uint8_t(chunk[j])) != 0) != neg) ++j;
            if (!j) break;
            if (!suppress) { uint32_t p = args.u32(); m.write(p, chunk.data(), j); m.w8(p + uint32_t(j), 0); ++assigned; }
            in += j;
        } else {
            break;
        }
        converted = true;
    }
    return assigned;
}

// ===================================================================== file system
namespace {

class Fs {
public:
    void init(const fs::path& data, const fs::path& sandbox) {
        data_ = data;
        sandbox_ = sandbox;
        fs::create_directories(sandbox_);
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(data_, ec)) {
            std::string n = e.path().filename().string(), low = n;
            for (char& ch : low) ch = char(std::tolower(uint8_t(ch)));
            index_[low] = n;
        }
    }

    std::string normalize(const std::string& guest) const {
        std::string g = guest;
        for (char& ch : g) if (ch == '\\') ch = '/';
        if (g.empty() || g[0] != '/') g = cwd + "/" + g;
        std::vector<std::string> parts;
        size_t i = 0;
        while (i <= g.size()) {
            size_t j = g.find('/', i);
            if (j == std::string::npos) j = g.size();
            std::string part = g.substr(i, j - i);
            if (part == "..") { if (!parts.empty()) parts.pop_back(); }
            else if (!part.empty() && part != ".") parts.push_back(part);
            i = j + 1;
        }
        std::string out;
        for (const auto& p : parts) out += "/" + p;
        return out.empty() ? "/" : out;
    }

    // -> read-only host path (may be empty) and the sandbox host path
    std::pair<fs::path, fs::path> map(const std::string& guest) const {
        const std::string g = normalize(guest);
        std::string low = g;
        for (char& ch : low) ch = char(std::tolower(uint8_t(ch)));
        static const char* kData[] = {"/sdcard/gameloft/games/gangstar2", "/mnt/sdcard/gameloft/games/gangstar2",
                                      "/storage/emulated/0/gameloft/games/gangstar2", "/sdcard/gameloft/games/gangster2"};
        for (const char* pre : kData) {
            size_t n = std::strlen(pre);
            if (low.compare(0, n, pre) == 0 && (low.size() == n || low[n] == '/')) {
                std::string rest = g.size() > n ? g.substr(n + 1) : "";
                fs::path ro;
                if (rest.empty()) ro = data_;
                else if (rest.find('/') == std::string::npos) {
                    std::string rl = low.substr(n + 1);
                    auto it = index_.find(rl);
                    if (it != index_.end()) ro = data_ / it->second;
                }
                return {ro, sandbox_ / "sdcard" / rest};
            }
        }
        static const std::string kSave = "/data/data/com.gameloft.android.gand.gloftgmhp.ml";
        if (low.compare(0, kSave.size(), kSave) == 0)
            return {{}, sandbox_ / "data" / (g.size() > kSave.size() ? g.substr(kSave.size() + 1) : "")};
        return {{}, sandbox_ / "other" / g.substr(1)};
    }

    fs::path resolve_read(const std::string& guest) const {
        auto [ro, sb] = map(guest);
        std::error_code ec;
        if (fs::exists(sb, ec)) return sb;
        if (!ro.empty() && fs::exists(ro, ec)) return ro;
        return {};
    }

    const fs::path& root() const { return root_; }
    void set_root(const fs::path& r) { root_ = r; }
    std::string cwd = "/";

private:
    fs::path data_, sandbox_, root_;
    std::unordered_map<std::string, std::string> index_;
};

struct GFile {
    FILE* fh = nullptr;
    bool writable = false;
    std::string guest_path;
    fs::path host_path;
};

struct DirStream {
    std::vector<std::string> names;
    size_t next = 0;
    uint32_t entry = 0;
    fs::path ro, sb;
};

Fs g_fs;
std::unordered_map<uint32_t, GFile> g_files;  // guest FILE* -> file
std::unordered_map<int, GFile> g_fds;         // fd -> file
std::unordered_map<uint32_t, DirStream> g_dirs;
int g_next_fd = 3;

std::string rel(const fs::path& p) {
    std::error_code ec;
    auto r = fs::relative(p, g_fs.root(), ec);
    return ec ? p.string() : r.string();
}

void fs_log(const std::string& msg) { LOG("[fs] %s", msg.c_str()); }

// Open through the overlay. Returns nullptr if not found (reads) / not creatable.
FILE* open_guest(const std::string& path, bool writable, bool truncate, bool append, fs::path* host) {
    auto [ro, sb] = g_fs.map(path);
    std::error_code ec;
    if (writable) {
        fs::create_directories(sb.parent_path(), ec);
        if (!truncate && !fs::exists(sb, ec)) {
            fs::path src = g_fs.resolve_read(path);
            if (!src.empty() && fs::is_regular_file(src, ec)) fs::copy_file(src, sb, ec);
        }
        const char* mode = append ? "ab+" : truncate ? "wb+" : (fs::exists(sb, ec) ? "rb+" : "wb+");
        *host = sb;
        return std::fopen(sb.string().c_str(), mode);
    }
    fs::path h = g_fs.resolve_read(path);
    if (h.empty() || fs::is_directory(h, ec)) return nullptr;
    *host = h;
    return std::fopen(h.string().c_str(), "rb");
}

const char* std_stream(Cpu& c, uint32_t fp) {
    uint32_t sf = c.rt().imports().data("__sF");
    if (fp == sf) return "stdin";
    if (fp == sf + 84) return "stdout";
    if (fp == sf + 168) return "stderr";
    return nullptr;
}

void emit(const char* stream, const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string::npos) j = text.size();
        if (j > i) LOG("[%s] %.*s", stream, int(j - i), text.data() + i);
        i = j + 1;
    }
}

GFile& file_of(uint32_t fp, const char* what) {
    auto it = g_files.find(fp);
    if (it == g_files.end()) throw std::runtime_error(std::string(what) + " on unknown FILE*");
    return it->second;
}

#define H(fn) void fn(Cpu& c, Args& a)
#define M (c.rt().mem())

// ---- printf family -----------------------------------------------------------------
uint32_t store(Cpu& c, uint32_t buf, int64_t size, const std::string& s) {
    if (size < 0) { M.write(buf, s.c_str(), s.size() + 1); }
    else if (size > 0) {
        size_t n = std::min<size_t>(s.size(), size_t(size) - 1);
        M.write(buf, s.data(), n);
        M.w8(buf + uint32_t(n), 0);
    }
    return uint32_t(s.size());
}
H(f_printf) { std::string s = cformat(c, M.cstr(a.u32()), a); emit("stdout", s); c.ret(uint32_t(s.size())); }
H(f_vprintf) { uint32_t f = a.u32(); Args va = Args::from_va_list(c, a.u32()); std::string s = cformat(c, M.cstr(f), va); emit("stdout", s); c.ret(uint32_t(s.size())); }
H(f_sprintf) { uint32_t b = a.u32(); uint32_t f = a.u32(); c.ret(store(c, b, -1, cformat(c, M.cstr(f), a))); }
H(f_snprintf) { uint32_t b = a.u32(), n = a.u32(), f = a.u32(); c.ret(store(c, b, n, cformat(c, M.cstr(f), a))); }
H(f_vsprintf) { uint32_t b = a.u32(), f = a.u32(); Args va = Args::from_va_list(c, a.u32()); c.ret(store(c, b, -1, cformat(c, M.cstr(f), va))); }
H(f_vsnprintf) { uint32_t b = a.u32(), n = a.u32(), f = a.u32(); Args va = Args::from_va_list(c, a.u32()); c.ret(store(c, b, n, cformat(c, M.cstr(f), va))); }
H(f_swprintf) {
    uint32_t b = a.u32(), n = a.u32(), f = a.u32();
    std::string fmt8 = utf8_of_wide(c, f);
    std::string s = cformat(c, fmt8.c_str(), a);
    // narrow -> wide (the game's wide strings are ASCII in practice; non-ASCII bytes pass through)
    uint32_t count = 0;
    for (; count < s.size() && count + 1 < n; ++count) M.w32(b + 4 * count, uint8_t(s[count]));
    if (n) M.w32(b + 4 * count, 0);
    c.ret(s.size() < n ? uint32_t(s.size()) : 0xFFFFFFFF);
}
H(f_sscanf) { uint32_t s = a.u32(), f = a.u32(); c.ret(uint32_t(csscanf(c, M.cstr(s), M.cstr(f), a))); }
H(f_puts) { emit("stdout", M.cstr(a.u32())); c.ret(1); }
H(f_putchar) { uint32_t ch = a.u32() & 0xFF; emit("stdout", std::string(1, char(ch))); c.ret(ch); }

// ---- liblog --------------------------------------------------------------------------
const char* prio_name(uint32_t p) {
    static const char* n[] = {"?", "?", "V", "D", "I", "W", "E", "F"};
    return p < 8 ? n[p] : "?";
}
H(l_print) {
    uint32_t prio = a.u32(), tag = a.u32(), fmt = a.u32();
    std::string msg = cformat(c, M.cstr(fmt), a);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    LOG("[log %s/%s] %s", prio_name(prio), tag ? M.cstr(tag) : "", msg.c_str());
    c.ret(1);
}
H(l_write) {
    uint32_t prio = a.u32(), tag = a.u32(), text = a.u32();
    LOG("[log %s/%s] %s", prio_name(prio), tag ? M.cstr(tag) : "", text ? M.cstr(text) : "");
    c.ret(1);
}

// ---- stdio ---------------------------------------------------------------------------------
H(s_fopen) {
    std::string path = M.cstr(a.u32()), mode = M.cstr(a.u32());
    bool w = mode.find_first_of("wa+") != std::string::npos;
    fs::path host;
    FILE* fh = open_guest(path, w, mode.find('w') != std::string::npos, mode.find('a') != std::string::npos, &host);
    if (!fh) {
        fs_log("fopen('" + path + "', '" + mode + "') -> NULL (not found)");
        M.w32(c.rt().errno_addr(), 2);
        return c.ret(0);
    }
    uint32_t fp = c.rt().heap().alloc(84);
    M.w16(fp + 12, w ? 0x10 : 0x04);
    M.w16(fp + 14, uint16_t(3 + g_files.size()));
    g_files[fp] = GFile{fh, w, path, host};
    fs_log("fopen('" + path + "', '" + mode + "') -> [" + rel(host) + "]");
    c.ret(fp);
}
H(s_fclose) {
    uint32_t fp = a.u32();
    auto it = g_files.find(fp);
    if (it != g_files.end()) { std::fclose(it->second.fh); g_files.erase(it); }
    c.ret(0);
}
H(s_fread) {
    uint32_t ptr = a.u32(), size = a.u32(), n = a.u32(), fp = a.u32();
    GFile& f = file_of(fp, "fread");
    c.ret(size && n ? uint32_t(std::fread(M.ptr(ptr), size, n, f.fh)) : 0);
}
H(s_fwrite) {
    uint32_t ptr = a.u32(), size = a.u32(), n = a.u32(), fp = a.u32();
    if (const char* st = std_stream(c, fp)) { emit(st, std::string(M.cstr(ptr), size_t(size) * n)); return c.ret(n); }
    GFile& f = file_of(fp, "fwrite");
    c.ret(f.writable && size && n ? uint32_t(std::fwrite(M.ptr(ptr), size, n, f.fh)) : 0);
}
H(s_fseek) { uint32_t fp = a.u32(); int32_t off = a.s32(); int wh = a.s32(); c.ret(uint32_t(std::fseek(file_of(fp, "fseek").fh, off, wh))); }
H(s_ftell) { c.ret(uint32_t(std::ftell(file_of(a.u32(), "ftell").fh))); }
H(s_fgetpos) { uint32_t fp = a.u32(), pos = a.u32(); M.w32(pos, uint32_t(std::ftell(file_of(fp, "fgetpos").fh))); c.ret(0); }
H(s_fsetpos) { uint32_t fp = a.u32(), pos = a.u32(); std::fseek(file_of(fp, "fsetpos").fh, int32_t(M.r32(pos)), SEEK_SET); c.ret(0); }
H(s_feof) { c.ret(std::feof(file_of(a.u32(), "feof").fh) ? 1 : 0); }
H(s_getc) { c.ret(uint32_t(std::fgetc(file_of(a.u32(), "getc").fh))); }
H(s_ungetc) { int ch = a.s32(); uint32_t fp = a.u32(); c.ret(uint32_t(std::ungetc(ch, file_of(fp, "ungetc").fh))); }
H(s_fflush) { c.ret(0); }
H(s_fputs) {
    uint32_t s = a.u32(), fp = a.u32();
    if (const char* st = std_stream(c, fp)) emit(st, M.cstr(s));
    else if (GFile& f = file_of(fp, "fputs"); f.writable) std::fputs(M.cstr(s), f.fh);
    c.ret(1);
}
H(s_putc) {
    uint32_t ch = a.u32() & 0xFF, fp = a.u32();
    if (const char* st = std_stream(c, fp)) emit(st, std::string(1, char(ch)));
    else if (GFile& f = file_of(fp, "putc"); f.writable) std::fputc(int(ch), f.fh);
    c.ret(ch);
}
H(s_fprintf) {
    uint32_t fp = a.u32(), fmt = a.u32();
    std::string s = cformat(c, M.cstr(fmt), a);
    if (const char* st = std_stream(c, fp)) emit(st, s);
    else if (GFile& f = file_of(fp, "fprintf"); f.writable) std::fwrite(s.data(), 1, s.size(), f.fh);
    c.ret(uint32_t(s.size()));
}

// ---- POSIX (O_* values are ARM Linux's) ------------------------------------------------------
H(p_open) {
    std::string path = M.cstr(a.u32());
    uint32_t flags = a.u32();
    bool w = flags & (1 | 2 | 0x40 | 0x200 | 0x400);
    fs::path host;
    FILE* fh = open_guest(path, w, flags & 0x200, flags & 0x400, &host);
    if (!fh) {
        fs_log("open('" + path + "') -> -1 (not found)");
        M.w32(c.rt().errno_addr(), 2);
        return c.ret(0xFFFFFFFF);
    }
    int fd = g_next_fd++;
    g_fds[fd] = GFile{fh, w, path, host};
    fs_log("open('" + path + "') -> fd " + std::to_string(fd) + " [" + rel(host) + "]");
    c.ret(uint32_t(fd));
}
H(p_read) {
    int fd = a.s32(); uint32_t buf = a.u32(), n = a.u32();
    auto it = g_fds.find(fd);
    if (it == g_fds.end()) return c.ret(0xFFFFFFFF);
    c.ret(n ? uint32_t(std::fread(M.ptr(buf), 1, n, it->second.fh)) : 0);
}
H(p_write) {
    int fd = a.s32(); uint32_t buf = a.u32(), n = a.u32();
    if (fd == 1 || fd == 2) { emit(fd == 1 ? "stdout" : "stderr", std::string(M.cstr(buf), n)); return c.ret(n); }
    auto it = g_fds.find(fd);
    if (it == g_fds.end() || !it->second.writable) return c.ret(0xFFFFFFFF);
    c.ret(uint32_t(std::fwrite(M.ptr(buf), 1, n, it->second.fh)));
}
H(p_lseek) {
    int fd = a.s32(); int32_t off = a.s32(); int wh = a.s32();
    auto it = g_fds.find(fd);
    if (it == g_fds.end() || std::fseek(it->second.fh, off, wh)) return c.ret(0xFFFFFFFF);
    c.ret(uint32_t(std::ftell(it->second.fh)));
}
H(p_close) {
    auto it = g_fds.find(a.s32());
    if (it != g_fds.end()) { std::fclose(it->second.fh); g_fds.erase(it); }
    c.ret(0);
}
void write_stat(Cpu& c, uint32_t buf, const fs::path& host) {
    std::error_code ec;
    uint8_t st[104] = {};
    bool dir = fs::is_directory(host, ec);
    uint32_t mode = dir ? 0040755 : 0100644, nlink = 1, blksize = 4096;
    int64_t size = dir ? 4096 : int64_t(fs::file_size(host, ec));
    uint64_t blocks = uint64_t(size + 511) / 512;
    auto ft = fs::last_write_time(host, ec);
    uint32_t mtime = uint32_t(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::clock_cast<std::chrono::system_clock>(ft).time_since_epoch()).count());
    std::memcpy(st + 16, &mode, 4);
    std::memcpy(st + 20, &nlink, 4);
    std::memcpy(st + 48, &size, 8);
    std::memcpy(st + 56, &blksize, 4);
    std::memcpy(st + 64, &blocks, 8);
    for (int off : {72, 80, 88}) std::memcpy(st + off, &mtime, 4);
    M.write(buf, st, sizeof st);
}
H(p_stat) {
    std::string path = M.cstr(a.u32());
    uint32_t buf = a.u32();
    fs::path host = g_fs.resolve_read(path);
    fs_log("stat('" + path + "') -> " + (host.empty() ? "-1" : "0"));
    if (host.empty()) { M.w32(c.rt().errno_addr(), 2); return c.ret(0xFFFFFFFF); }
    write_stat(c, buf, host);
    c.ret(0);
}
H(p_fstat) {
    int fd = a.s32(); uint32_t buf = a.u32();
    auto it = g_fds.find(fd);
    if (it == g_fds.end()) return c.ret(0xFFFFFFFF);
    write_stat(c, buf, it->second.host_path);
    c.ret(0);
}
H(p_fcntl) { c.ret(0); }
H(p_mkdir) {
    std::string path = M.cstr(a.u32());
    std::error_code ec;
    fs::create_directories(g_fs.map(path).second, ec);
    fs_log("mkdir('" + path + "') -> 0 [sandbox]");
    c.ret(0);
}
H(p_remove) {
    std::string path = M.cstr(a.u32());
    std::error_code ec;
    fs::path sb = g_fs.map(path).second;
    bool ok = fs::is_regular_file(sb, ec) && fs::remove(sb, ec);
    fs_log("remove('" + path + "') -> " + (ok ? "0 [sandbox]" : "-1 (only sandbox files can be removed)"));
    c.ret(ok ? 0 : 0xFFFFFFFF);
}
H(p_chdir) {
    std::string path = M.cstr(a.u32());
    g_fs.cwd = g_fs.normalize(path);
    fs_log("chdir('" + path + "') -> 0 (cwd now " + g_fs.cwd + ")");
    c.ret(0);
}
H(p_opendir) {
    std::string path = M.cstr(a.u32());
    auto [ro, sb] = g_fs.map(path);
    std::error_code ec;
    std::set<std::string> names;
    for (const fs::path& d : {ro, sb})
        if (!d.empty() && fs::is_directory(d, ec))
            for (const auto& e : fs::directory_iterator(d, ec)) names.insert(e.path().filename().string());
    if (names.empty() && !(fs::is_directory(ro, ec) || fs::is_directory(sb, ec))) {
        fs_log("opendir('" + path + "') -> NULL");
        return c.ret(0);
    }
    uint32_t h = c.rt().heap().alloc(4);
    g_dirs[h] = DirStream{{names.begin(), names.end()}, 0, c.rt().heap().alloc(280), ro, sb};
    fs_log("opendir('" + path + "') -> " + std::to_string(names.size()) + " entries");
    c.ret(h);
}
H(p_readdir) {
    auto it = g_dirs.find(a.u32());
    if (it == g_dirs.end() || it->second.next >= it->second.names.size()) return c.ret(0);
    DirStream& d = it->second;
    const std::string& name = d.names[d.next++];
    std::error_code ec;
    bool dir = fs::is_directory(d.sb / name, ec) || (!d.ro.empty() && fs::is_directory(d.ro / name, ec));
    uint8_t ent[280] = {};
    uint64_t ino = d.next;
    uint16_t reclen = 280;
    std::memcpy(ent, &ino, 8);
    std::memcpy(ent + 8, &ino, 8);
    std::memcpy(ent + 16, &reclen, 2);
    ent[18] = dir ? 4 : 8;
    std::memcpy(ent + 19, name.data(), std::min<size_t>(name.size(), 255));
    M.write(d.entry, ent, sizeof ent);
    c.ret(d.entry);
}
H(p_closedir) { g_dirs.erase(a.u32()); c.ret(0); }

#undef H
#undef M

}  // namespace

void register_libc_stdio(rt::Runtime& rt) {
    g_fs.init(rt.paths().data, rt.paths().sandbox);
    g_fs.set_root(rt.paths().root);
    const std::pair<const char*, cpu::Handler> table[] = {
        {"printf", f_printf}, {"vprintf", f_vprintf}, {"sprintf", f_sprintf}, {"snprintf", f_snprintf},
        {"vsprintf", f_vsprintf}, {"vsnprintf", f_vsnprintf}, {"swprintf", f_swprintf}, {"sscanf", f_sscanf},
        {"puts", f_puts}, {"putchar", f_putchar},
        {"__android_log_print", l_print}, {"__android_log_write", l_write},
        {"fopen", s_fopen}, {"fclose", s_fclose}, {"fread", s_fread}, {"fwrite", s_fwrite}, {"fseek", s_fseek},
        {"ftell", s_ftell}, {"fgetpos", s_fgetpos}, {"fsetpos", s_fsetpos}, {"feof", s_feof}, {"getc", s_getc},
        {"ungetc", s_ungetc}, {"fflush", s_fflush}, {"setvbuf", s_fflush}, {"fputs", s_fputs}, {"putc", s_putc},
        {"fprintf", s_fprintf},
        {"open", p_open}, {"read", p_read}, {"write", p_write}, {"lseek", p_lseek}, {"close", p_close},
        {"stat", p_stat}, {"fstat", p_fstat}, {"fcntl", p_fcntl}, {"mkdir", p_mkdir}, {"remove", p_remove},
        {"chdir", p_chdir}, {"opendir", p_opendir}, {"readdir", p_readdir}, {"closedir", p_closedir},
    };
    for (const auto& [name, h] : table) rt.set_handler(name, h);
}

}  // namespace hle
