// `gangstar --selftest-load`: the C++ twin of `tools/probe/probe.py 1a`.
// Loads and relocates libGangster2.so into guest memory and verifies the result
// against facts that do not come from the relocation code itself.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <string>

#include "core/guest_memory.h"
#include "core/layout.h"
#include "core/log.h"
#include "hle/imports.h"
#include "loader/elf_image.h"
#include "rt/runtime.h"

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    LOG("%s  %s", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

}  // namespace

// `gangstar --selftest-fastpath`: every guest-side helper (src/cpu/fastpath.S), run inside
// the emulated Cortex-A15, must match host IEEE arithmetic bit for bit (any NaN matches any NaN).
int run_selftest_fastpath(const rt::Paths& paths) {
    rt::Runtime rt;
    std::string err;
    if (!rt.init(paths, &err, true)) { LOG("runtime init failed: %s", err.c_str()); return 1; }
    cpu::Cpu& c = rt.main_cpu();
    // Resolve helper addresses through the import table (fastpath map).
    auto fp = [&](const char* n) -> uint32_t {
        loader::Sym s;
        s.name = n;
        s.type = loader::STT_FUNC;
        uint32_t a = rt.imports().resolve(s);
        if (!rt.imports().is_fastpath(n)) FATAL("%s is not a fastpath helper", n);
        return a;
    };
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    auto next = [&] { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    const uint32_t fspecial[] = {0, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000,
                                 0x00000001, 0x007FFFFF, 0x7F7FFFFF, 0x4F000000, 0xCF000000, 0x5F800000, 0x3EAAAAAB};
    const uint64_t dspecial[] = {0, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
                                 0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 1,
                                 0x000FFFFFFFFFFFFFull, 0x7FEFFFFFFFFFFFFFull, 0x41E0000000000000ull,
                                 0xC1E0000000000000ull, 0x41F0000000000000ull};
    auto f32 = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
    auto f64 = [](uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; };
    auto b32 = [](float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; };
    auto b64 = [](double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; };
    auto sat32 = [](double x) -> int32_t { if (x != x) return 0; if (x >= 2147483647.0) return INT32_MAX; if (x <= -2147483648.0) return INT32_MIN; return int32_t(x); };
    auto satu32 = [](double x) -> uint32_t { if (x != x || x <= 0) return 0; if (x >= 4294967295.0) return 0xFFFFFFFFu; return uint32_t(x); };
    auto same_f = [&](uint32_t g, float want) { return std::isnan(want) ? std::isnan(f32(g)) : g == b32(want); };
    auto same_d = [&](uint64_t g, double want) { return std::isnan(want) ? std::isnan(f64(g)) : g == b64(want); };

    int fails = 0;
    auto report = [&](const char* name, int bad, int total) {
        LOG("%s  %-18s %d/%d", bad ? "FAIL" : "PASS", name, total - bad, total);
        fails += bad != 0;
    };
    const int N = 4000;
    auto fin = [&](int i) { return i < int(std::size(fspecial)) ? fspecial[i] : uint32_t(next()); };
    auto din = [&](int i) { return i < int(std::size(dspecial)) ? dspecial[i] : next(); };

    // binary float ops
    struct FOp { const char* n; float (*f)(float, float); };
    const FOp fops[] = {{"__aeabi_fadd", [](float a, float b) { return a + b; }},
                        {"__aeabi_fsub", [](float a, float b) { return a - b; }},
                        {"__aeabi_fmul", [](float a, float b) { return a * b; }},
                        {"__aeabi_fdiv", [](float a, float b) { return a / b; }}};
    for (const FOp& op : fops) {
        uint32_t a_ = fp(op.n);
        int bad = 0, n = 0;
        for (int i = 0; i < N; ++i, ++n) {
            // every special x special pair first, then random bit patterns
            uint32_t x = i < 196 ? fspecial[i % 14] : uint32_t(next()), y = i < 196 ? fspecial[i / 14] : uint32_t(next());
            uint32_t r = uint32_t(c.call(a_, {x, y}));
            if (!same_f(r, op.f(f32(x), f32(y)))) ++bad;
        }
        report(op.n, bad, n);
    }
    struct FCmp { const char* n; bool (*f)(float, float); };
    const FCmp fcmps[] = {{"__aeabi_fcmpeq", [](float a, float b) { return a == b; }},
                          {"__aeabi_fcmplt", [](float a, float b) { return a < b; }},
                          {"__aeabi_fcmple", [](float a, float b) { return a <= b; }},
                          {"__aeabi_fcmpge", [](float a, float b) { return a >= b; }},
                          {"__aeabi_fcmpgt", [](float a, float b) { return a > b; }}};
    for (const FCmp& op : fcmps) {
        uint32_t a_ = fp(op.n);
        int bad = 0, n = 0;
        for (int i = 0; i < N; ++i, ++n) {
            uint32_t x = i < 196 ? fspecial[i % 14] : uint32_t(next()), y = i < 196 ? fspecial[i / 14] : (i & 1 ? x : uint32_t(next()));
            bad += uint32_t(c.call(a_, {x, y})) != uint32_t(op.f(f32(x), f32(y)));
        }
        report(op.n, bad, n);
    }
    // binary double ops
    struct DOp { const char* n; double (*f)(double, double); };
    const DOp dops[] = {{"__aeabi_dadd", [](double a, double b) { return a + b; }},
                        {"__aeabi_dsub", [](double a, double b) { return a - b; }},
                        {"__aeabi_dmul", [](double a, double b) { return a * b; }},
                        {"__aeabi_ddiv", [](double a, double b) { return a / b; }}};
    for (const DOp& op : dops) {
        uint32_t a_ = fp(op.n);
        int bad = 0, n = 0;
        for (int i = 0; i < N; ++i, ++n) {
            uint64_t x = i < 169 ? dspecial[i % 13] : next(), y = i < 169 ? dspecial[i / 13] : next();
            uint64_t r = c.call(a_, {uint32_t(x), uint32_t(x >> 32), uint32_t(y), uint32_t(y >> 32)});
            if (!same_d(r, op.f(f64(x), f64(y)))) ++bad;
        }
        report(op.n, bad, n);
    }
    struct DCmp { const char* n; bool (*f)(double, double); };
    const DCmp dcmps[] = {{"__aeabi_dcmpeq", [](double a, double b) { return a == b; }},
                          {"__aeabi_dcmplt", [](double a, double b) { return a < b; }},
                          {"__aeabi_dcmple", [](double a, double b) { return a <= b; }},
                          {"__aeabi_dcmpge", [](double a, double b) { return a >= b; }},
                          {"__aeabi_dcmpgt", [](double a, double b) { return a > b; }},
                          {"__aeabi_dcmpun", [](double a, double b) { return a != a || b != b; }}};
    for (const DCmp& op : dcmps) {
        uint32_t a_ = fp(op.n);
        int bad = 0, n = 0;
        for (int i = 0; i < N; ++i, ++n) {
            uint64_t x = i < 169 ? dspecial[i % 13] : next(), y = i < 169 ? dspecial[i / 13] : (i & 1 ? x : next());
            bad += uint32_t(c.call(a_, {uint32_t(x), uint32_t(x >> 32), uint32_t(y), uint32_t(y >> 32)})) != uint32_t(op.f(f64(x), f64(y)));
        }
        report(op.n, bad, n);
    }
    // conversions
    auto conv = [&](const char* n, auto in, auto check) {
        uint32_t a_ = fp(n);
        int bad = 0;
        for (int i = 0; i < N; ++i) {
            auto x = in(i);
            uint64_t r = c.call(a_, {uint32_t(uint64_t(x)), uint32_t(uint64_t(x) >> 32)});
            bad += !check(x, r);
        }
        report(n, bad, N);
    };
    conv("__aeabi_f2iz", fin, [&](uint32_t x, uint64_t r) { return uint32_t(r) == uint32_t(sat32(f32(x))); });
    conv("__aeabi_f2d", fin, [&](uint32_t x, uint64_t r) { return same_d(r, double(f32(x))); });
    conv("__aeabi_i2f", [&](int) { return uint32_t(next()); }, [&](uint32_t x, uint64_t r) { return same_f(uint32_t(r), float(int32_t(x))); });
    conv("__aeabi_ui2f", [&](int) { return uint32_t(next()); }, [&](uint32_t x, uint64_t r) { return same_f(uint32_t(r), float(x)); });
    conv("__aeabi_i2d", [&](int) { return uint32_t(next()); }, [&](uint32_t x, uint64_t r) { return same_d(r, double(int32_t(x))); });
    conv("__aeabi_ui2d", [&](int) { return uint32_t(next()); }, [&](uint32_t x, uint64_t r) { return same_d(r, double(x)); });
    conv("__aeabi_d2f", din, [&](uint64_t x, uint64_t r) { return same_f(uint32_t(r), float(f64(x))); });
    conv("__aeabi_d2iz", din, [&](uint64_t x, uint64_t r) { return uint32_t(r) == uint32_t(sat32(f64(x))); });
    conv("__aeabi_d2uiz", din, [&](uint64_t x, uint64_t r) { return uint32_t(r) == satu32(f64(x)); });
    conv("sqrtf", fin, [&](uint32_t x, uint64_t r) { return same_f(uint32_t(r), std::sqrt(f32(x))); });
    conv("sqrt", din, [&](uint64_t x, uint64_t r) { return same_d(r, std::sqrt(f64(x))); });
    // integer division and 64-bit multiply
    {
        uint32_t idiv = fp("__aeabi_idiv"), uidiv = fp("__aeabi_uidiv"), idm = fp("__aeabi_idivmod"),
                 uidm = fp("__aeabi_uidivmod"), lmul = fp("__aeabi_lmul");
        int b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;
        for (int i = 0; i < N; ++i) {
            int32_t x = int32_t(next()), y = int32_t(next() >> (next() % 32));
            if (i % 50 == 0) y = -1, x = INT32_MIN;
            if (y == 0) y = 7;
            int32_t q = (x == INT32_MIN && y == -1) ? x : x / y, r = (x == INT32_MIN && y == -1) ? 0 : x % y;
            b1 += int32_t(c.call(idiv, {uint32_t(x), uint32_t(y)})) != q;
            uint64_t qr = c.call(idm, {uint32_t(x), uint32_t(y)});
            b3 += int32_t(qr) != q || int32_t(qr >> 32) != r;
            uint32_t ux = uint32_t(x), uy = uint32_t(y);
            b2 += uint32_t(c.call(uidiv, {ux, uy})) != ux / uy;
            uint64_t uqr = c.call(uidm, {ux, uy});
            b4 += uint32_t(uqr) != ux / uy || uint32_t(uqr >> 32) != ux % uy;
            uint64_t p = next(), q2 = next();
            b5 += c.call(lmul, {uint32_t(p), uint32_t(p >> 32), uint32_t(q2), uint32_t(q2 >> 32)}) != p * q2;
        }
        report("__aeabi_idiv", b1, N);
        report("__aeabi_uidiv", b2, N);
        report("__aeabi_idivmod", b3, N);
        report("__aeabi_uidivmod", b4, N);
        report("__aeabi_lmul", b5, N);
        bool z = c.call(idiv, {5, 0}) == 0 && uint32_t(c.call(uidiv, {5, 0})) == 0;
        report("division by zero -> 0", z ? 0 : 1, 1);
    }
    LOG("%s (%d helpers failed)", fails ? "FASTPATH SELFTEST FAILED" : "FASTPATH SELFTEST PASSED", fails);
    return fails ? 1 : 0;
}

int run_selftest_load(const std::string& so_path) {
    using namespace loader;
    core::GuestMemory mem;
    if (!mem.init()) return 1;
    hle::ImportTable imports;
    if (!imports.init(mem)) return 1;

    ElfImage elf;
    std::string err;
    if (!elf.open(so_path, &err)) {
        LOG("cannot load %s: %s", so_path.c_str(), err.c_str());
        return 1;
    }
    // Map each PT_LOAD's page range with its own protection.
    for (const LoadSeg& s : elf.loads()) {
        uint32_t lo = s.vaddr & ~0xFFFu, hi = (s.vaddr + s.memsz + 0xFFF) & ~0xFFFu;
        uint32_t prot = (s.flags & 4 ? core::PROT_R : 0) | (s.flags & 2 ? core::PROT_W : 0) | (s.flags & 1 ? core::PROT_X : 0);
        if (!mem.map(layout::kLibBase + lo, hi - lo, prot, s.flags & 1 ? "lib RX" : "lib RW")) return 1;
    }
    RelocStats st;
    bool ok = elf.relocate_into(mem, layout::kLibBase, [&](const Sym& s) { return imports.resolve(s); }, &st, &err);
    check(ok, "relocate_into: " + (ok ? std::string("ok") : err));
    if (!ok) return 1;

    const uint32_t relcount = elf.dyn(0x6ffffffa);  // DT_RELCOUNT
    const uint32_t n_rel = elf.dyn(18) / elf.dyn(19, 8), n_plt = elf.dyn(2) / 8;
    check(st.relative == relcount && relcount == 43374,
          fmt("R_ARM_RELATIVE applied = %u = DT_RELCOUNT (%u)", st.relative, relcount));
    check(st.relative + st.abs32 + st.glob_dat == n_rel,
          fmt(".rel.dyn entries applied = %u (DT_RELSZ/DT_RELENT); ABS32=%u, GLOB_DAT=%u", n_rel, st.abs32, st.glob_dat));
    check(st.jump_slot == n_plt && n_plt == 290, fmt("R_ARM_JUMP_SLOT applied = %u = DT_PLTRELSZ/8 (%u)", st.jump_slot, n_plt));

    // Imports
    int n_imports = 0, n_data = 0, n_traps = 0;
    std::set<std::string> unbound;
    for (const Sym& s : elf.dynsyms()) {
        if (s.name.empty() || !s.undefined()) continue;
        ++n_imports;
        if (imports.is_data(s.name)) ++n_data;
        else if (imports.trap_addr(s.name)) ++n_traps;
        else unbound.insert(s.name);
    }
    check(n_imports == 299 && n_traps == 290 && n_data == 7 && imports.unresolved_objects().empty() &&
              unbound == std::set<std::string>{"__aeabi_unwind_cpp_pr0", "__aeabi_unwind_cpp_pr1"},
          fmt("%d imports: %d function traps + %d bionic data objects bound; %zu never referenced by a relocation",
              n_imports, n_traps, n_data, unbound.size()));

    // GOT slots
    int bad = 0;
    for (const std::vector<Rel>* t : {&elf.rel_dyn(), &elf.rel_plt()}) {
        for (const Rel& r : *t) {
            if (r.type != R_ARM_GLOB_DAT && r.type != R_ARM_JUMP_SLOT) continue;
            uint32_t v = mem.r32(layout::kLibBase + r.offset);
            const Sym& s = elf.dynsyms()[r.sym];
            bool in_trap = v >= layout::kTrapBase && v < layout::kTrapBase + layout::kTrapSize;
            bool in_data = v >= layout::kDataBase && v < layout::kDataBase + layout::kDataSize;
            bool in_lib = v >= layout::kLibBase && v < layout::kLibBase + elf.image_size();
            bool good = s.undefined() ? (r.type == R_ARM_JUMP_SLOT ? in_trap : in_trap || in_data) : in_lib;
            bad += !good;
        }
    }
    check(bad == 0, fmt("every JUMP_SLOT/GLOB_DAT slot points at a trap, a data object or the library (%d bad)", bad));

    // init_array
    const uint32_t ia = elf.dyn(25), ia_n = elf.dyn(27) / 4;  // DT_INIT_ARRAY, DT_INIT_ARRAYSZ
    int bad_ia = 0;
    for (uint32_t i = 0; i < ia_n; ++i) {
        uint32_t v = mem.r32(layout::kLibBase + ia + 4 * i);
        const Sym* f = elf.func_starting_at((v - layout::kLibBase) & ~1u);
        bad_ia += !f || ((f->value ^ v) & 1);
    }
    check(ia_n == 448 && bad_ia == 0,
          fmt("%u DT_INIT_ARRAY entries all point at function starts with matching ARM/Thumb bit (%d bad)", ia_n, bad_ia));

    // vtables
    std::set<uint32_t> typeinfos;
    for (const Sym& s : elf.symtab())
        if (s.type == STT_OBJECT && s.name.rfind("_ZTI", 0) == 0) typeinfos.insert(s.value);
    std::map<std::string, int> cats;
    int vtables = 0, slots = 0, unexplained = 0;
    for (const Sym& s : elf.symtab()) {
        if (s.type != STT_OBJECT || s.name.rfind("_ZTV", 0) != 0 || s.size < 8) continue;
        ++vtables;
        for (uint32_t i = 0; i < s.size / 4; ++i, ++slots) {
            uint32_t v = mem.r32(layout::kLibBase + s.value + 4 * i);
            int32_t sv = int32_t(v);
            if (v == 0) { ++cats["zero"]; continue; }
            if (imports.trap_index(v) >= 0) { ++cats["import"]; continue; }
            if (v >= layout::kLibBase && v < layout::kLibBase + elf.image_size()) {
                uint32_t rel = v - layout::kLibBase;
                const Sym* f = elf.func_starting_at(rel & ~1u);
                if (f && !((f->value ^ v) & 1)) { ++cats["function"]; continue; }
                if (typeinfos.count(rel)) { ++cats["typeinfo"]; continue; }
            } else if (sv > -0x100000 && sv < 0x100000) {
                ++cats["offset"]; continue;
            }
            ++unexplained;
        }
    }
    check(vtables == 1456 && unexplained == 0,
          fmt("%d C++ vtables, %d slots: every slot is a function start, typeinfo, import, 0 or small offset (%d unexplained)",
              vtables, slots, unexplained));
    for (auto& [k, v] : cats) LOG("        %s: %d", k.c_str(), v);

    // Same function bytes as seen in Phase 0
    uint32_t a = elf.func("Java_com_gameloft_android_GAND_GloftGMHP_ML_GameRenderer_nativeGetJNIEnv");
    const uint8_t* p = mem.ptr(layout::kLibBase + (a & ~1u));
    static const uint8_t expect[] = {0x02, 0x4b, 0x03, 0x4a, 0x7b, 0x44};
    check(a == 0x368db9 && std::memcmp(p, expect, sizeof expect) == 0,
          fmt("nativeGetJNIEnv at lib+%#x starts with ldr r3,[pc,#8]; ldr r2,[pc,#12]; add r3,pc", a));

    LOG("%s (%d failed)", g_fail ? "SELFTEST FAILED" : "SELFTEST PASSED", g_fail);
    return g_fail ? 1 : 0;
}
