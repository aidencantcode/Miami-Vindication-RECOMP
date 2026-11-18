// `gangstar --selftest-load`: the C++ twin of `tools/probe/probe.py 1a`.
// Loads and relocates libGangster2.so into guest memory and verifies the result
// against facts that do not come from the relocation code itself.
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include "core/guest_memory.h"
#include "core/layout.h"
#include "core/log.h"
#include "hle/imports.h"
#include "loader/elf_image.h"

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
