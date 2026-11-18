// Bionic (Android 2.x, 32-bit ARM) data objects imported directly by the game.
// Layouts from bionic's OpenBSD-derived headers of that era, confirmed in the probe
// (analysis/probe_2a.md): `_ctype_` and `_tolower_tab_` are read as POINTERS to tables.
//
//   __sF               FILE[3] (stdin, stdout, stderr); sizeof(FILE) = 84,
//                      _flags (short) @12, _file (short) @14
//   _ctype_            const char*  -> 1+256 byte table (entry 0 = EOF)
//   _tolower_tab_      const short* -> 1+256 shorts
//   _toupper_tab_      const short* -> 1+256 shorts
//   __stack_chk_guard  canary word
//   __dso_handle, std::nothrow: only the address matters
#include <cctype>

#include "hle/imports.h"

namespace hle {

namespace {
constexpr uint8_t U = 0x01, L = 0x02, N = 0x04, S = 0x08, P = 0x10, C = 0x20, X = 0x40, B = 0x80;
constexpr uint32_t kFileSize = 84;

uint8_t ctype_bits(int c) {
    uint8_t f = 0;
    if (c < 0x20 || c == 0x7F) f |= C;
    if (c >= 0x09 && c <= 0x0D) f |= S;
    if (c == 0x20) f |= S | B;
    if (c >= 0x21 && c <= 0x7E && !std::isalnum(c)) f |= P;
    if (c >= '0' && c <= '9') f |= N;
    if (c >= 'A' && c <= 'Z') f |= U;
    if (c >= 'a' && c <= 'z') f |= L;
    if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) f |= X;
    return f;
}
}  // namespace

std::unordered_map<std::string, uint32_t> build_bionic_data(core::GuestMemory& mem, core::BumpAllocator& alloc) {
    std::unordered_map<std::string, uint32_t> out;

    uint32_t sf = alloc.alloc(3 * kFileSize, 8);
    const int16_t flags[3] = {0x0004, 0x0008, 0x0008 | 0x0002};  // __SRD, __SWR, __SWR|__SNBF
    for (int i = 0; i < 3; ++i) {
        mem.w16(sf + i * kFileSize + 12, uint16_t(flags[i]));
        mem.w16(sf + i * kFileSize + 14, uint16_t(i));
    }
    out["__sF"] = sf;

    auto pointer_to = [&](const char* name, uint32_t table) {
        uint32_t p = alloc.alloc(4, 4);
        mem.w32(p, table);
        out[name] = p;
        out[std::string(name) + "#table"] = table;
    };

    uint32_t ct = alloc.alloc(257, 8);
    mem.w8(ct, 0);
    for (int c = 0; c < 256; ++c) mem.w8(ct + 1 + c, ctype_bits(c));
    pointer_to("_ctype_", ct);

    for (int upper = 0; upper < 2; ++upper) {
        uint32_t t = alloc.alloc(257 * 2, 8);
        mem.w16(t, uint16_t(-1));
        for (int c = 0; c < 256; ++c) {
            int v = c;
            if (upper && c >= 'a' && c <= 'z') v = c - 32;
            if (!upper && c >= 'A' && c <= 'Z') v = c + 32;
            mem.w16(t + 2 + 2 * c, uint16_t(v));
        }
        pointer_to(upper ? "_toupper_tab_" : "_tolower_tab_", t);
    }

    uint32_t guard = alloc.alloc(4, 4);
    mem.w32(guard, 0xC0DEC0DE);
    out["__stack_chk_guard"] = guard;
    out["__dso_handle"] = alloc.alloc(4, 4);
    out["_ZSt7nothrow"] = alloc.alloc(4, 4);
    return out;
}

}  // namespace hle
