// HLE for bionic libc/libm/libstdc++ imports: ARM EABI runtime (soft-float, integer
// division), libm, memory/strings, number parsing, C++ runtime support, process,
// time, and (for now single-threaded) pthreads. C++ twin of tools/probe/hle_libc.py.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "core/layout.h"
#include "core/log.h"
#include "hle/hle.h"
#include "rt/runtime.h"

namespace hle {

using cpu::Args;
using cpu::Cpu;

namespace {

#define H(fn) void fn(Cpu& c, Args& a)

template <class T>
T bits_to(uint64_t b) {
    T v;
    std::memcpy(&v, &b, sizeof v);
    return v;
}

int32_t sat32(double x) {
    if (x != x) return 0;
    if (x >= 2147483647.0) return std::numeric_limits<int32_t>::max();
    if (x <= -2147483648.0) return std::numeric_limits<int32_t>::min();
    return int32_t(x);
}
int64_t sat64(double x) {
    if (x != x) return 0;
    if (x >= 9223372036854775807.0) return std::numeric_limits<int64_t>::max();
    if (x <= -9223372036854775808.0) return std::numeric_limits<int64_t>::min();
    return int64_t(x);
}
uint32_t satu32(double x) {
    if (x != x || x <= 0) return 0;
    if (x >= 4294967295.0) return 0xFFFFFFFFu;
    return uint32_t(x);
}

void div0(Cpu& c, const char* what) {
    LOG("WARNING: %s: integer division by zero (returning 0) [from %s]", what, c.where(c.reg(cpu::LR)).c_str());
}

// ---- soft-float -----------------------------------------------------------------
H(fadd) { float x = a.f32(), y = a.f32(); c.ret_f(x + y); }
H(fsub) { float x = a.f32(), y = a.f32(); c.ret_f(x - y); }
H(fmul) { float x = a.f32(), y = a.f32(); c.ret_f(x * y); }
H(fdiv) { float x = a.f32(), y = a.f32(); c.ret_f(x / y); }
H(fcmpeq) { float x = a.f32(), y = a.f32(); c.ret(x == y); }
H(fcmplt) { float x = a.f32(), y = a.f32(); c.ret(x < y); }
H(fcmple) { float x = a.f32(), y = a.f32(); c.ret(x <= y); }
H(fcmpge) { float x = a.f32(), y = a.f32(); c.ret(x >= y); }
H(fcmpgt) { float x = a.f32(), y = a.f32(); c.ret(x > y); }
H(f2iz) { c.ret(uint32_t(sat32(a.f32()))); }
H(f2lz) { c.ret64(uint64_t(sat64(a.f32()))); }
H(f2d) { c.ret_d(double(a.f32())); }
H(i2f) { c.ret_f(float(a.s32())); }
H(ui2f) { c.ret_f(float(a.u32())); }
H(l2f) { c.ret_f(float(a.s64())); }
H(dadd) { double x = a.f64(), y = a.f64(); c.ret_d(x + y); }
H(dsub) { double x = a.f64(), y = a.f64(); c.ret_d(x - y); }
H(dmul) { double x = a.f64(), y = a.f64(); c.ret_d(x * y); }
H(ddiv) { double x = a.f64(), y = a.f64(); c.ret_d(x / y); }
H(dcmpeq) { double x = a.f64(), y = a.f64(); c.ret(x == y); }
H(dcmplt) { double x = a.f64(), y = a.f64(); c.ret(x < y); }
H(dcmple) { double x = a.f64(), y = a.f64(); c.ret(x <= y); }
H(dcmpge) { double x = a.f64(), y = a.f64(); c.ret(x >= y); }
H(dcmpgt) { double x = a.f64(), y = a.f64(); c.ret(x > y); }
H(dcmpun) { double x = a.f64(), y = a.f64(); c.ret(x != x || y != y); }
H(d2f) { c.ret_f(float(a.f64())); }
H(d2iz) { c.ret(uint32_t(sat32(a.f64()))); }
H(d2uiz) { c.ret(satu32(a.f64())); }
H(d2lz) { c.ret64(uint64_t(sat64(a.f64()))); }
H(i2d) { c.ret_d(double(a.s32())); }
H(ui2d) { c.ret_d(double(a.u32())); }
H(ul2d) { c.ret_d(double(a.u64())); }

// ---- integer division (truncating, like ARM's helpers) -----------------------------
H(idiv) {
    int32_t x = a.s32(), y = a.s32();
    if (!y) return div0(c, "__aeabi_idiv"), c.ret(0);
    c.ret(uint32_t(x == INT32_MIN && y == -1 ? x : x / y));
}
H(idivmod) {
    int32_t x = a.s32(), y = a.s32();
    if (!y) return div0(c, "__aeabi_idivmod"), c.ret64(0);
    if (x == INT32_MIN && y == -1) return c.ret64(uint32_t(x));
    c.set_reg(cpu::R0, uint32_t(x / y));
    c.set_reg(cpu::R1, uint32_t(x % y));
}
H(uidiv) {
    uint32_t x = a.u32(), y = a.u32();
    if (!y) return div0(c, "__aeabi_uidiv"), c.ret(0);
    c.ret(x / y);
}
H(uidivmod) {
    uint32_t x = a.u32(), y = a.u32();
    if (!y) return div0(c, "__aeabi_uidivmod"), c.ret64(0);
    c.set_reg(cpu::R0, x / y);
    c.set_reg(cpu::R1, x % y);
}
H(ldivmod) {
    int64_t x = a.s64(), y = a.s64();
    int64_t q = 0, r = 0;
    if (!y) div0(c, "__aeabi_ldivmod");
    else if (x == INT64_MIN && y == -1) q = x;
    else q = x / y, r = x % y;
    c.set_reg(cpu::R0, uint32_t(q)); c.set_reg(cpu::R1, uint32_t(uint64_t(q) >> 32));
    c.set_reg(cpu::R2, uint32_t(r)); c.set_reg(cpu::R3, uint32_t(uint64_t(r) >> 32));
}
H(uldivmod) {
    uint64_t x = a.u64(), y = a.u64();
    uint64_t q = 0, r = 0;
    if (!y) div0(c, "__aeabi_uldivmod");
    else q = x / y, r = x % y;
    c.set_reg(cpu::R0, uint32_t(q)); c.set_reg(cpu::R1, uint32_t(q >> 32));
    c.set_reg(cpu::R2, uint32_t(r)); c.set_reg(cpu::R3, uint32_t(r >> 32));
}
H(lmul) { uint64_t x = a.u64(), y = a.u64(); c.ret64(x * y); }

// ---- libm ---------------------------------------------------------------------------
#define MATH_F1(name, fn) H(name) { c.ret_f(fn(a.f32())); }
#define MATH_D1(name, fn) H(name) { c.ret_d(fn(a.f64())); }
MATH_D1(m_acos, std::acos) MATH_F1(m_acosf, std::acos) MATH_D1(m_asin, std::asin) MATH_F1(m_asinf, std::asin)
MATH_D1(m_atan, std::atan) MATH_F1(m_atanf, std::atan) MATH_D1(m_ceil, std::ceil) MATH_F1(m_ceilf, std::ceil)
MATH_D1(m_cos, std::cos) MATH_F1(m_cosf, std::cos) MATH_D1(m_exp, std::exp) MATH_F1(m_expf, std::exp)
MATH_D1(m_floor, std::floor) MATH_F1(m_floorf, std::floor) MATH_D1(m_log, std::log) MATH_F1(m_logf, std::log)
MATH_D1(m_log10, std::log10) MATH_D1(m_sin, std::sin) MATH_F1(m_sinf, std::sin) MATH_D1(m_sqrt, std::sqrt)
MATH_F1(m_sqrtf, std::sqrt) MATH_D1(m_tan, std::tan) MATH_F1(m_tanf, std::tan)
H(m_atan2) { double y = a.f64(), x = a.f64(); c.ret_d(std::atan2(y, x)); }
H(m_atan2f) { float y = a.f32(), x = a.f32(); c.ret_f(std::atan2(y, x)); }
H(m_pow) { double x = a.f64(), y = a.f64(); c.ret_d(std::pow(x, y)); }
H(m_powf) { float x = a.f32(), y = a.f32(); c.ret_f(std::pow(x, y)); }
H(m_fmod) { double x = a.f64(), y = a.f64(); c.ret_d(std::fmod(x, y)); }
H(m_fmodf) { float x = a.f32(), y = a.f32(); c.ret_f(std::fmod(x, y)); }
H(m_ldexpf) { float x = a.f32(); int e = a.s32(); c.ret_f(std::ldexp(x, e)); }
H(m_isfinitef) { c.ret(std::isfinite(a.f32())); }

// ---- memory and strings (fastmem: guest pointers -> host pointers) -----------------
#define M (c.rt().mem())
H(s_memcpy) { uint32_t d = a.u32(), s = a.u32(), n = a.u32(); if (n) std::memmove(M.ptr(d), M.ptr(s), n); c.ret(d); }
H(s_memset) { uint32_t d = a.u32(); int v = int(a.u32() & 0xFF); uint32_t n = a.u32(); if (n) std::memset(M.ptr(d), v, n); c.ret(d); }
H(s_memcmp) { uint32_t x = a.u32(), y = a.u32(), n = a.u32(); c.ret(uint32_t(n ? std::memcmp(M.ptr(x), M.ptr(y), n) : 0)); }
H(s_memchr) {
    uint32_t s = a.u32(); int v = int(a.u32() & 0xFF); uint32_t n = a.u32();
    const void* p = n ? std::memchr(M.ptr(s), v, n) : nullptr;
    c.ret(p ? s + uint32_t(static_cast<const uint8_t*>(p) - M.ptr(s)) : 0);
}
H(s_strlen) { c.ret(uint32_t(std::strlen(M.cstr(a.u32())))); }
H(s_strcmp) { uint32_t x = a.u32(), y = a.u32(); c.ret(uint32_t(std::strcmp(M.cstr(x), M.cstr(y)))); }
H(s_strncmp) { uint32_t x = a.u32(), y = a.u32(), n = a.u32(); c.ret(uint32_t(std::strncmp(M.cstr(x), M.cstr(y), n))); }
H(s_strcasecmp) { uint32_t x = a.u32(), y = a.u32(); c.ret(uint32_t(_stricmp(M.cstr(x), M.cstr(y)))); }
H(s_strncasecmp) { uint32_t x = a.u32(), y = a.u32(), n = a.u32(); c.ret(uint32_t(_strnicmp(M.cstr(x), M.cstr(y), n))); }
H(s_strcpy) { uint32_t d = a.u32(), s = a.u32(); std::memmove(M.ptr(d), M.ptr(s), std::strlen(M.cstr(s)) + 1); c.ret(d); }
H(s_strncpy) {
    uint32_t d = a.u32(), s = a.u32(), n = a.u32();
    size_t len = strnlen(M.cstr(s), n);
    std::memmove(M.ptr(d), M.ptr(s), len);
    if (len < n) std::memset(M.ptr(d + uint32_t(len)), 0, n - len);
    c.ret(d);
}
H(s_strcat) {
    uint32_t d = a.u32(), s = a.u32();
    uint32_t end = d + uint32_t(std::strlen(M.cstr(d)));
    std::memmove(M.ptr(end), M.ptr(s), std::strlen(M.cstr(s)) + 1);
    c.ret(d);
}
H(s_strncat) {
    uint32_t d = a.u32(), s = a.u32(), n = a.u32();
    uint32_t end = d + uint32_t(std::strlen(M.cstr(d)));
    size_t len = strnlen(M.cstr(s), n);
    std::memmove(M.ptr(end), M.ptr(s), len);
    M.w8(end + uint32_t(len), 0);
    c.ret(d);
}
uint32_t guest_of(Cpu& c, const void* host) { return uint32_t(static_cast<const uint8_t*>(host) - c.rt().mem().base()); }
H(s_strchr) { uint32_t s = a.u32(); int v = int(a.u32() & 0xFF); const char* p = std::strchr(M.cstr(s), v); c.ret(p ? guest_of(c, p) : 0); }
H(s_strrchr) { uint32_t s = a.u32(); int v = int(a.u32() & 0xFF); const char* p = std::strrchr(M.cstr(s), v); c.ret(p ? guest_of(c, p) : 0); }
H(s_strstr) { uint32_t s = a.u32(), t = a.u32(); const char* p = std::strstr(M.cstr(s), M.cstr(t)); c.ret(p ? guest_of(c, p) : 0); }
H(s_strcspn) { uint32_t s = a.u32(), r = a.u32(); c.ret(uint32_t(std::strcspn(M.cstr(s), M.cstr(r)))); }
H(s_strdup) {
    uint32_t s = a.u32();
    uint32_t n = uint32_t(std::strlen(M.cstr(s))) + 1;
    uint32_t p = game_malloc(c, n);  // the game frees it with its own free()
    if (p) std::memcpy(M.ptr(p), M.ptr(s), n);
    c.ret(p);
}
uint32_t wlen(Cpu& c, uint32_t p) { uint32_t n = 0; while (M.r32(p + 4 * n)) ++n; return n; }
H(s_wcslen) { c.ret(wlen(c, a.u32())); }
H(s_wcscpy) { uint32_t d = a.u32(), s = a.u32(); std::memmove(M.ptr(d), M.ptr(s), 4 * (wlen(c, s) + 1)); c.ret(d); }
H(s_wcscmp) {
    uint32_t x = a.u32(), y = a.u32();
    for (;; x += 4, y += 4) {
        uint32_t p = M.r32(x), q = M.r32(y);
        if (p != q) return c.ret(p < q ? 0xFFFFFFFFu : 1);
        if (!p) return c.ret(0);
    }
}

// ---- number parsing (host CRT; `long` is 32-bit on Windows, as in the guest) --------
H(n_strtol) {
    uint32_t p = a.u32(), endp = a.u32(); int base = a.s32();
    char* end;
    long v = std::strtol(M.cstr(p), &end, base);
    if (endp) M.w32(endp, guest_of(c, end));
    c.ret(uint32_t(v));
}
H(n_strtoul) {
    uint32_t p = a.u32(), endp = a.u32(); int base = a.s32();
    char* end;
    unsigned long v = std::strtoul(M.cstr(p), &end, base);
    if (endp) M.w32(endp, guest_of(c, end));
    c.ret(uint32_t(v));
}
H(n_strtod) {
    uint32_t p = a.u32(), endp = a.u32();
    char* end;
    double v = std::strtod(M.cstr(p), &end);
    if (endp) M.w32(endp, guest_of(c, end));
    c.ret_d(v);
}
H(n_atoi) { c.ret(uint32_t(std::atoi(M.cstr(a.u32())))); }

// ---- C++ runtime ----------------------------------------------------------------------
H(cxx_guard_acquire) { uint32_t g = a.u32(); c.ret(M.r32(g) & 1 ? 0 : 1); }
H(cxx_guard_release) { M.w32(a.u32(), 1); c.ret(0); }
H(cxx_atexit) { a.u32(); a.u32(); c.ret(0); }  // destructors never run (the process just exits)
H(cxx_pure_virtual) { throw std::runtime_error("pure virtual function called from " + c.where(c.reg(cpu::LR))); }
H(cxx_new_array_nothrow) { c.redirect(c.rt().lib_func("_Znaj")); }  // -> the game's operator new[]

// ---- process / environment ---------------------------------------------------------------
H(p_errno) { c.ret(c.rt().errno_addr()); }
H(p_sbrk) {
    int32_t incr = a.s32();
    uint32_t old = c.rt().sbrk(incr);
    if (old == 0xFFFFFFFF) {
        LOG("WARNING: sbrk(%d) failed (heap limit)", incr);
        M.w32(c.rt().errno_addr(), 12);
    }
    c.ret(old);
}
H(p_mmap) {
    uint32_t addr = a.u32(), len = a.u32(), prot = a.u32(), flags = a.u32();
    int32_t fd = a.s32();
    (void)addr; (void)prot;
    if (!(flags & 0x20)) {  // only MAP_ANONYMOUS so far
        LOG("STUB: mmap of fd %d not implemented", fd);
        return c.ret(0xFFFFFFFF);
    }
    c.ret(c.rt().mmap_anon(len));
}
H(p_munmap) { c.ret(0); }
H(p_abort) { throw std::runtime_error("abort() called from " + c.where(c.reg(cpu::LR))); }
H(p_exit) { int code = a.s32(); throw std::runtime_error("exit(" + std::to_string(code) + ") called from " + c.where(c.reg(cpu::LR))); }
H(p_stack_chk_fail) { throw std::runtime_error("__stack_chk_fail: stack smashing detected in " + c.where(c.reg(cpu::LR))); }
H(p_getenv) { LOG("[env] getenv(\"%s\") -> NULL", M.cstr(a.u32())); c.ret(0); }
H(p_setlocale) {
    static uint32_t s_c = 0;
    if (!s_c) { s_c = c.rt().heap().alloc(2); M.write(s_c, "C", 2); }
    c.ret(s_c);
}
H(p_sysconf) {
    // bionic libc/include/sys/sysconf.h values; 0x27/0x28 confirmed by the game's own calls
    static const std::unordered_map<uint32_t, uint32_t> kv = {{0x06, 100}, {0x27, 4096}, {0x28, 4096}, {0x60, 2},
                                                              {0x61, 2}, {0x62, 131072}, {0x63, 65536}};
    uint32_t name = a.u32();
    auto it = kv.find(name);
    if (it == kv.end()) LOG("[env] sysconf(%#x) -> UNKNOWN NAME, -1", name);
    c.ret(it == kv.end() ? 0xFFFFFFFF : it->second);
}

// ---- time -------------------------------------------------------------------------------------
H(t_time) {
    uint32_t t = uint32_t(std::time(nullptr));
    uint32_t p = a.u32();
    if (p) M.w32(p, t);
    c.ret(t);
}
H(t_gettimeofday) {
    uint32_t tv = a.u32();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (tv) { M.w32(tv, uint32_t(us / 1000000)); M.w32(tv + 4, uint32_t(us % 1000000)); }
    c.ret(0);
}
void write_tm(Cpu& c, uint32_t p, const std::tm& t) {
    // bionic struct tm: 9 ints + long tm_gmtoff + const char* tm_zone = 44 bytes
    const int32_t v[11] = {t.tm_sec, t.tm_min,  t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year,
                           t.tm_wday, t.tm_yday, t.tm_isdst > 0 ? 1 : 0, 0, 0};
    M.write(p, v, sizeof v);
}
H(t_localtime) {
    static uint32_t buf = 0;
    if (!buf) buf = c.rt().heap().alloc(44);
    std::time_t t = int32_t(M.r32(a.u32()));
    std::tm tm{};
    localtime_s(&tm, &t);
    write_tm(c, buf, tm);
    c.ret(buf);
}
H(t_gmtime) {
    static uint32_t buf = 0;
    if (!buf) buf = c.rt().heap().alloc(44);
    std::time_t t = int32_t(M.r32(a.u32()));
    std::tm tm{};
    gmtime_s(&tm, &t);
    write_tm(c, buf, tm);
    c.ret(buf);
}
H(t_mktime) {
    int32_t v[9];
    M.read(a.u32(), v, sizeof v);
    std::tm tm{};
    tm.tm_sec = v[0]; tm.tm_min = v[1]; tm.tm_hour = v[2]; tm.tm_mday = v[3]; tm.tm_mon = v[4]; tm.tm_year = v[5];
    tm.tm_isdst = -1;
    c.ret(uint32_t(std::mktime(&tm)));
}
H(t_sleep) { c.ret(0); }  // the frame loop paces itself; guest sleeps are not needed yet

// ---- misc ----------------------------------------------------------------------------------------
uint64_t g_rand48 = 0x1234ABCD330Eull;
H(r_lrand48) { g_rand48 = (0x5DEECE66Dull * g_rand48 + 0xB) & ((1ull << 48) - 1); c.ret(uint32_t(g_rand48 >> 17)); }
H(r_srand48) { g_rand48 = ((uint64_t(a.u32()) << 16) | 0x330E) & ((1ull << 48) - 1); c.ret(0); }
H(q_qsort) {
    uint32_t base = a.u32(), n = a.u32(), size = a.u32(), cmp = a.u32();
    if (n < 2 || !size) return c.ret(0);
    std::vector<uint32_t> order(n);
    for (uint32_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t i, uint32_t j) {
        return int32_t(c.call(cmp, {base + i * size, base + j * size})) < 0;
    });
    std::vector<uint8_t> tmp(size_t(n) * size);
    for (uint32_t i = 0; i < n; ++i) std::memcpy(&tmp[size_t(i) * size], M.ptr(base + order[i] * size), size);
    std::memcpy(M.ptr(base), tmp.data(), tmp.size());
    c.ret(0);
}

struct JmpBuf { uint32_t r[8], sp, lr; };
std::unordered_map<uint32_t, JmpBuf> g_jmpbufs;
H(j_setjmp) {
    uint32_t buf = a.u32();
    JmpBuf j;
    for (int i = 0; i < 8; ++i) j.r[i] = c.reg(cpu::Reg(cpu::R4 + i));
    j.sp = c.reg(cpu::SP);
    j.lr = c.reg(cpu::LR);
    g_jmpbufs[buf] = j;
    M.w32(buf, 0x4A4D5042);
    c.ret(0);
}
H(j_longjmp) {
    uint32_t buf = a.u32(), val = a.u32();
    auto it = g_jmpbufs.find(buf);
    if (it == g_jmpbufs.end()) throw std::runtime_error("longjmp to unknown jmp_buf");
    for (int i = 0; i < 8; ++i) c.set_reg(cpu::Reg(cpu::R4 + i), it->second.r[i]);
    c.set_reg(cpu::SP, it->second.sp);
    c.ret(val ? val : 1);
    c.redirect(it->second.lr);
}

// ---- pthreads (single guest thread for now; NOTES: the vox audio thread is not started yet) ----
H(pt_ok) { c.ret(0); }
H(pt_cond_wait) { LOG("WARNING: pthread_cond_wait with a single guest thread [from %s]", c.where(c.reg(cpu::LR)).c_str()); c.ret(0); }
H(pt_cond_timedwait) { c.ret(110); }  // ETIMEDOUT
H(pt_self) { c.ret(0x1000); }
H(pt_equal) { uint32_t x = a.u32(), y = a.u32(); c.ret(x == y); }
std::vector<uint32_t> g_tls;
H(pt_key_create) { uint32_t kp = a.u32(); M.w32(kp, uint32_t(g_tls.size())); g_tls.push_back(0); c.ret(0); }
H(pt_getspecific) { uint32_t k = a.u32(); c.ret(k < g_tls.size() ? g_tls[k] : 0); }
H(pt_setspecific) { uint32_t k = a.u32(), v = a.u32(); if (k < g_tls.size()) g_tls[k] = v; c.ret(0); }
H(pt_create) {
    uint32_t th = a.u32(), attr = a.u32(), start = a.u32(), arg = a.u32();
    (void)attr;
    LOG("STUB: pthread_create(start=%s, arg=%#x) -> thread NOT started (single guest thread)", c.where(start).c_str(), arg);
    if (th) M.w32(th, 0x2000);
    c.ret(0);
}
#undef M

}  // namespace

uint32_t game_malloc(Cpu& c, uint32_t size) { return uint32_t(c.call(c.rt().lib_func("malloc"), {size})); }

void register_libc_core(rt::Runtime& rt) {
    const std::pair<const char*, cpu::Handler> table[] = {
        {"__aeabi_fadd", fadd}, {"__aeabi_fsub", fsub}, {"__aeabi_fmul", fmul}, {"__aeabi_fdiv", fdiv},
        {"__aeabi_fcmpeq", fcmpeq}, {"__aeabi_fcmplt", fcmplt}, {"__aeabi_fcmple", fcmple},
        {"__aeabi_fcmpge", fcmpge}, {"__aeabi_fcmpgt", fcmpgt}, {"__aeabi_f2iz", f2iz}, {"__aeabi_f2lz", f2lz},
        {"__aeabi_f2d", f2d}, {"__aeabi_i2f", i2f}, {"__aeabi_ui2f", ui2f}, {"__aeabi_l2f", l2f},
        {"__aeabi_dadd", dadd}, {"__aeabi_dsub", dsub}, {"__aeabi_dmul", dmul}, {"__aeabi_ddiv", ddiv},
        {"__aeabi_dcmpeq", dcmpeq}, {"__aeabi_dcmplt", dcmplt}, {"__aeabi_dcmple", dcmple},
        {"__aeabi_dcmpge", dcmpge}, {"__aeabi_dcmpgt", dcmpgt}, {"__aeabi_dcmpun", dcmpun},
        {"__aeabi_d2f", d2f}, {"__aeabi_d2iz", d2iz}, {"__aeabi_d2uiz", d2uiz}, {"__aeabi_d2lz", d2lz},
        {"__aeabi_i2d", i2d}, {"__aeabi_ui2d", ui2d}, {"__aeabi_ul2d", ul2d},
        {"__aeabi_idiv", idiv}, {"__aeabi_idivmod", idivmod}, {"__aeabi_uidiv", uidiv},
        {"__aeabi_uidivmod", uidivmod}, {"__aeabi_ldivmod", ldivmod}, {"__aeabi_uldivmod", uldivmod},
        {"__aeabi_lmul", lmul},
        {"acos", m_acos}, {"acosf", m_acosf}, {"asin", m_asin}, {"asinf", m_asinf}, {"atan", m_atan},
        {"atanf", m_atanf}, {"atan2", m_atan2}, {"atan2f", m_atan2f}, {"ceil", m_ceil}, {"ceilf", m_ceilf},
        {"cos", m_cos}, {"cosf", m_cosf}, {"exp", m_exp}, {"expf", m_expf}, {"floor", m_floor},
        {"floorf", m_floorf}, {"fmod", m_fmod}, {"fmodf", m_fmodf}, {"ldexpf", m_ldexpf}, {"log", m_log},
        {"log10", m_log10}, {"logf", m_logf}, {"pow", m_pow}, {"powf", m_powf}, {"sin", m_sin},
        {"sinf", m_sinf}, {"sqrt", m_sqrt}, {"sqrtf", m_sqrtf}, {"tan", m_tan}, {"tanf", m_tanf},
        {"__isfinitef", m_isfinitef},
        {"memcpy", s_memcpy}, {"memmove", s_memcpy}, {"memset", s_memset}, {"memcmp", s_memcmp},
        {"memchr", s_memchr}, {"strlen", s_strlen}, {"strcmp", s_strcmp}, {"strncmp", s_strncmp},
        {"strcasecmp", s_strcasecmp}, {"strncasecmp", s_strncasecmp}, {"strcpy", s_strcpy},
        {"strncpy", s_strncpy}, {"strcat", s_strcat}, {"strncat", s_strncat}, {"strchr", s_strchr},
        {"strrchr", s_strrchr}, {"strstr", s_strstr}, {"strcspn", s_strcspn}, {"strdup", s_strdup},
        {"wcslen", s_wcslen}, {"wcscpy", s_wcscpy}, {"wcscmp", s_wcscmp},
        {"strtol", n_strtol}, {"strtoul", n_strtoul}, {"strtod", n_strtod}, {"atoi", n_atoi}, {"atol", n_atoi},
        {"__cxa_guard_acquire", cxx_guard_acquire}, {"__cxa_guard_release", cxx_guard_release},
        {"__aeabi_atexit", cxx_atexit}, {"__cxa_pure_virtual", cxx_pure_virtual},
        {"_ZnajRKSt9nothrow_t", cxx_new_array_nothrow},
        {"__errno", p_errno}, {"sbrk", p_sbrk}, {"mmap", p_mmap}, {"munmap", p_munmap}, {"abort", p_abort},
        {"exit", p_exit}, {"__stack_chk_fail", p_stack_chk_fail}, {"getenv", p_getenv},
        {"setlocale", p_setlocale}, {"sysconf", p_sysconf},
        {"time", t_time}, {"gettimeofday", t_gettimeofday}, {"localtime", t_localtime}, {"gmtime", t_gmtime},
        {"mktime", t_mktime}, {"sleep", t_sleep}, {"usleep", t_sleep}, {"sched_yield", t_sleep},
        {"lrand48", r_lrand48}, {"srand48", r_srand48}, {"qsort", q_qsort},
        {"setjmp", j_setjmp}, {"longjmp", j_longjmp},
        {"pthread_mutex_init", pt_ok}, {"pthread_mutex_lock", pt_ok}, {"pthread_mutex_unlock", pt_ok},
        {"pthread_mutex_trylock", pt_ok}, {"pthread_mutex_destroy", pt_ok}, {"pthread_mutexattr_init", pt_ok},
        {"pthread_mutexattr_settype", pt_ok}, {"pthread_mutexattr_destroy", pt_ok},
        {"pthread_cond_init", pt_ok}, {"pthread_cond_destroy", pt_ok}, {"pthread_cond_signal", pt_ok},
        {"pthread_cond_broadcast", pt_ok}, {"pthread_attr_init", pt_ok}, {"pthread_attr_destroy", pt_ok},
        {"pthread_attr_setdetachstate", pt_ok}, {"pthread_attr_setstacksize", pt_ok},
        {"pthread_setschedparam", pt_ok}, {"pthread_getschedparam", pt_ok},
        {"sched_get_priority_max", pt_ok}, {"sched_get_priority_min", pt_ok},
        {"pthread_cond_wait", pt_cond_wait}, {"pthread_cond_timedwait", pt_cond_timedwait},
        {"pthread_self", pt_self}, {"pthread_equal", pt_equal}, {"pthread_key_create", pt_key_create},
        {"pthread_getspecific", pt_getspecific}, {"pthread_setspecific", pt_setspecific},
        {"pthread_create", pt_create}, {"pthread_join", pt_ok},
    };
    for (const auto& [name, h] : table) rt.set_handler(name, h);
}

}  // namespace hle
