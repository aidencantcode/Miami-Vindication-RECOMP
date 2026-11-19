#include "recomp/rc_runtime.h"

#include <algorithm>
#include <csetjmp>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/layout.h"
#include "core/log.h"
#include "cpu/cpu.h"
#define RC_INTERFACE_ONLY
#include "recomp/rc.h"
#include "rt/runtime.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

rt::Runtime* g_rt = nullptr;
cpu::Cpu* g_cpu = nullptr;
RcState g_state{};
std::vector<int> g_import_trap;  // import index -> trap index (-1: not a function trap)
uint64_t g_indirect = 0, g_imports = 0;

struct CacheEntry {
    uint32_t addr;
    RcFn fn;
};
CacheEntry g_cache[8192];

std::string where(uint32_t a) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%08x", a);
    std::string s = buf;
    if (a >= RC_LIB_BASE) {
        const uint32_t* end = rc_fn_addr + rc_fn_count;
        const uint32_t* it = std::upper_bound(rc_fn_addr, end, a);
        if (it != rc_fn_addr) {
            const size_t i = size_t(it - rc_fn_addr) - 1;
            char off[24];
            std::snprintf(off, sizeof off, "+%#x", a - rc_fn_addr[i]);
            s += std::string(" (") + rc_fn_name[i] + off + ")";
        }
    }
    return s;
}

RcFn lookup(uint32_t a) {
    const uint32_t* end = rc_fn_addr + rc_fn_count;
    const uint32_t* it = std::lower_bound(rc_fn_addr, end, a);
    return it != end && *it == a ? rc_fn_ptr[it - rc_fn_addr] : nullptr;
}

// ---- setjmp / longjmp ----------------------------------------------------------------------------
// Generated code calls `setjmp(*rc_setjmp_slot(s))` in the frame of the guest function that called
// setjmp, so a guest longjmp becomes a host longjmp back into that frame. The guest registers that
// setjmp preserves (r4-r11, sp) are saved per guest jmp_buf address.
struct JmpSlot {
    jmp_buf host;
    uint32_t r4_r11[8];
    uint32_t sp;
};
std::unordered_map<uint32_t, std::unique_ptr<JmpSlot>> g_jmp;
int g_longjmp_trap = -1;

[[noreturn]] void do_longjmp(RcState* s) {
    const uint32_t buf = s->r[0], val = s->r[1];
    auto it = g_jmp.find(buf);
    if (it == g_jmp.end()) throw std::runtime_error("recompiled code: longjmp to a jmp_buf setjmp never saw");
    JmpSlot& j = *it->second;
    std::memcpy(&s->r[4], j.r4_r11, sizeof j.r4_r11);
    s->r[13] = j.sp;
    s->r[0] = val ? val : 1;
    std::longjmp(j.host, 1);
}

void dispatch_trap(RcState* s, int trap) {
    if (trap <= 0) return;  // trap 0 = return-to-host sentinel: nothing to do here
    if (trap == g_longjmp_trap) do_longjmp(s);  // direct or through a pointer (libpng's longjmp_fn)
    ++g_imports;
    cpu::Args args(*g_cpu);
    g_rt->dispatch(*g_cpu, trap, args);
    uint32_t to;
    if (g_cpu->take_redirect(&to)) {
        // A handler that tail-jumps into game code (operator new[] nothrow -> operator new[]).
        // longjmp also redirects (to a saved return address): not supported here yet.
        if (!lookup(to & ~1u))
            throw std::runtime_error("recompiled code: handler redirected into the middle of code at " + where(to) +
                                     " (longjmp?)");
        rc_call(s, to);
    }
}

#ifdef _WIN32
// ---- crash reports ------------------------------------------------------------------------------
// Recompiled code touches guest memory directly, so a bad guest pointer is a host access violation
// (the 4 GB guest reservation is PAGE_NOACCESS where nothing is mapped). The unhandled-exception
// filter names the faulting guest address, maps host code addresses back to recompiled functions
// (nearest function entry at or below), and walks the host stack for the guest call chain.
std::vector<std::pair<uintptr_t, uint32_t>> g_host_fns;  // host entry -> index in rc_fn_*

uintptr_t resolve_thunk(uintptr_t p) {
    // /INCREMENTAL: function pointers point at `jmp rel32` thunks in the ILT.
    for (int i = 0; i < 4 && *reinterpret_cast<const uint8_t*>(p) == 0xE9; ++i) {
        int32_t rel;
        std::memcpy(&rel, reinterpret_cast<const void*>(p + 1), 4);
        p = p + 5 + intptr_t(rel);
    }
    return p;
}

std::string host_where(uintptr_t rip) {
    if (g_host_fns.empty()) {
        g_host_fns.reserve(rc_fn_count);
        for (uint32_t i = 0; i < rc_fn_count; ++i)
            g_host_fns.emplace_back(resolve_thunk(reinterpret_cast<uintptr_t>(rc_fn_ptr[i])), i);
        std::sort(g_host_fns.begin(), g_host_fns.end());
    }
    char buf[160];
    auto it = std::upper_bound(g_host_fns.begin(), g_host_fns.end(), std::make_pair(rip, UINT32_MAX));
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(rip), &mod);
    char name[MAX_PATH] = "?";
    if (mod) {
        GetModuleFileNameA(mod, name, MAX_PATH);
        const char* slash = std::strrchr(name, '\\');
        if (slash) std::memmove(name, slash + 1, std::strlen(slash));
    }
    const uintptr_t lo = g_host_fns.front().first, hi = g_host_fns.back().first + 0x100000;
    if (it != g_host_fns.begin() && rip >= lo && rip < hi) {
        --it;
        std::snprintf(buf, sizeof buf, "%s+%#llx  guest %08x %s (+%#llx host)", name,
                      (unsigned long long)(rip - uintptr_t(mod)), rc_fn_addr[it->second], rc_fn_name[it->second],
                      (unsigned long long)(rip - it->first));
    } else {
        std::snprintf(buf, sizeof buf, "%s+%#llx", name, (unsigned long long)(rip - uintptr_t(mod)));
    }
    return buf;
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    LOG("recomp: host exception %#lx at %s", er->ExceptionCode,
        host_where(reinterpret_cast<uintptr_t>(er->ExceptionAddress)).c_str());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const uintptr_t a = er->ExceptionInformation[1];
        const uintptr_t base = reinterpret_cast<uintptr_t>(g_state.mem);
        const char* kind = er->ExceptionInformation[0] == 1 ? "write" : er->ExceptionInformation[0] == 8 ? "execute" : "read";
        if (a >= base && a - base < (uintptr_t(1) << 32))
            LOG("recomp:   guest %s of unmapped address %08llx", kind, (unsigned long long)(a - base));
        else
            LOG("recomp:   host %s of %#llx (not guest memory)", kind, (unsigned long long)a);
    }
    const uint32_t* r = g_state.r;
    LOG("recomp:   r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x", r[0], r[1], r[2], r[3], r[4],
        r[5], r[6], r[7]);
    LOG("recomp:   r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x sp=%08x lr=%08x", r[8], r[9], r[10], r[11], r[12],
        r[13], r[14]);
    CONTEXT ctx = *ep->ContextRecord;
    for (int depth = 0; depth < 40 && ctx.Rip; ++depth) {
        LOG("recomp:   #%-2d %s", depth, host_where(ctx.Rip).c_str());
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
        if (!fe) {  // leaf function: return address on top of the stack
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fe, &ctx, &handler_data, &frame, nullptr);
    }
    std::fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- stack sampler (GANGSTAR_RC_SAMPLE=<seconds>) ---------------------------------------------
// For hangs: after the given time, suspend the main thread a few times and log its host stack as
// recompiled functions. Nothing is allocated or logged while the thread is suspended (it may hold
// the heap or log lock); the raw return addresses are collected first and formatted afterwards.
HANDLE g_main_thread = nullptr;

int walk_stack(CONTEXT ctx, uintptr_t* out, int max) {
    int n = 0;
    for (; n < max && ctx.Rip; ++n) {
        out[n] = ctx.Rip;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
        if (!fe) {
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fe, &ctx, &handler_data, &frame, nullptr);
    }
    return n;
}

DWORD WINAPI sampler_thread(void* param) {
    const DWORD first_ms = DWORD(reinterpret_cast<uintptr_t>(param));
    host_where(0);  // build the host map now, not while the main thread is suspended
    Sleep(first_ms);
    for (int sample = 0; sample < 4; ++sample) {
        uintptr_t pcs[48];
        int n = 0;
        if (SuspendThread(g_main_thread) != DWORD(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(g_main_thread, &ctx)) n = walk_stack(ctx, pcs, 48);
            ResumeThread(g_main_thread);
        }
        LOG("recomp: sample %d of the main thread (%d frames), guest r0=%08x r1=%08x lr=%08x:", sample, n,
            g_state.r[0], g_state.r[1], g_state.r[14]);
        for (int i = 0; i < n; ++i) LOG("recomp:   #%-2d %s", i, host_where(pcs[i]).c_str());
        Sleep(700);
    }
    return 0;
}
#endif

}  // namespace

extern "C" void rc_call(RcState* s, uint32_t target) {
    const uint32_t a = target & ~1u;
    CacheEntry& e = g_cache[(a >> 1) & 8191];
    ++g_indirect;
    if (e.addr == a && e.fn) {
        e.fn(s);
        return;
    }
    if (a >= layout::kTrapBase && a < layout::kTrapBase + layout::kTrapSize) {
        dispatch_trap(s, g_rt->imports().trap_index(a));
        return;
    }
    RcFn fn = lookup(a);
    if (!fn) throw std::runtime_error("recompiled code: call to unknown address " + where(target));
    e = {a, fn};
    fn(s);
}

extern "C" void rc_import(RcState* s, int idx) {
    const int trap = idx >= 0 && size_t(idx) < g_import_trap.size() ? g_import_trap[idx] : -1;
    if (trap < 0)
        throw std::runtime_error(std::string("recompiled code: import without a handler: ") +
                                 (idx >= 0 && uint32_t(idx) < rc_import_count ? rc_import_name[idx] : "?"));
    dispatch_trap(s, trap);
}

// Function hooks (mods, debug probes): codegen wraps every function in its HOOKABLE list as
//   if (rc_hook_enter(s, addr)) return;  body(s);  rc_hook_leave(s, addr);
// and these run the hooks registered with rt::Runtime::hook_function, like the Unicorn code hooks.
extern "C" int rc_hook_enter(RcState* s, uint32_t addr) {
    const int idx = g_rt->hook_index(addr);
    if (idx < 0) return 0;
    if (!g_rt->run_pre_hook(*g_cpu, idx)) return 0;
    uint32_t ignored;
    g_cpu->take_redirect(&ignored);  // a pre hook that skips may also have "returned" via PC
    (void)s;
    return 1;
}

extern "C" void rc_hook_leave(RcState* s, uint32_t addr) {
    const int idx = g_rt->hook_index(addr);
    if (idx >= 0) g_rt->run_post_hook(*g_cpu, idx);
    (void)s;
}

extern "C" void* rc_setjmp_slot(RcState* s) {
    std::unique_ptr<JmpSlot>& slot = g_jmp[s->r[0]];
    if (!slot) slot = std::make_unique<JmpSlot>();
    std::memcpy(slot->r4_r11, &s->r[4], sizeof slot->r4_r11);
    slot->sp = s->r[13];
    s->r[0] = 0;
    return &slot->host;
}

extern "C" void rc_bad_jump(RcState*, uint32_t site, uint32_t target) {
    throw std::runtime_error("recompiled code: jump-table target " + where(target) + " not in the table at " +
                             where(site));
}

extern "C" void rc_fell_off(RcState*, uint32_t func) {
    throw std::runtime_error("recompiled code: ran off the end of " + where(func));
}

extern "C" void rc_unhandled(RcState*, uint32_t addr, const char* what) {
    throw std::runtime_error(std::string("recompiled code: unhandled ") + what + " at " + where(addr));
}

namespace recomp {

bool available() { return true; }

bool bind(rt::Runtime& rt, cpu::Cpu& c, std::string* err) {
    g_rt = &rt;
    g_cpu = &c;
    g_state = {};
    g_state.mem = rt.mem().base();
    g_import_trap.assign(rc_import_count, -1);
    int missing = 0;
    for (uint32_t i = 0; i < rc_import_count; ++i) {
        const uint32_t a = rt.imports().trap_addr(rc_import_name[i]);
        if (a) g_import_trap[i] = rt.imports().trap_index(a);
        else ++missing;
    }
    std::fill(std::begin(g_cache), std::end(g_cache), CacheEntry{0, nullptr});
    const uint32_t lj = rt.imports().trap_addr("longjmp");
    g_longjmp_trap = lj ? rt.imports().trap_index(lj) : -1;
    g_jmp.clear();
    c.attach_recomp(&g_state);
    cpu::Cpu::set_recomp_call(&rc_call);
#ifdef _WIN32
    SetUnhandledExceptionFilter(&on_unhandled);
    // Unicorn enforces guest page protection; fastmem host pages are all read/write. Make the
    // read-only guest regions (code + rodata) read-only on the host too, so a stray write from
    // recompiled code faults right where it happens instead of corrupting a string or constant.
    int ro = 0;
    for (const core::Region& r : rt.mem().regions()) {
        if (r.prot & core::PROT_W) continue;
        DWORD old = 0;
        if (VirtualProtect(rt.mem().base() + r.addr, r.size, PAGE_READONLY, &old)) ++ro;
        else LOG("recomp: VirtualProtect(%s) failed (%lu)", r.name.c_str(), GetLastError());
    }
    LOG("recomp: %d read-only guest regions protected on the host", ro);
    if (const char* env = std::getenv("GANGSTAR_RC_SAMPLE")) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        const uintptr_t ms = uintptr_t(std::atof(env) * 1000.0);
        CloseHandle(CreateThread(nullptr, 0, &sampler_thread, reinterpret_cast<void*>(ms), 0, nullptr));
        LOG("recomp: stack sampler armed (%s s)", env);
    }
#endif
    LOG("recomp: %u functions bound, %u imports (%d without a trap: data imports)", rc_fn_count, rc_import_count,
        missing);
    (void)err;
    return true;
}

void take_stats(uint64_t* indirect_calls, uint64_t* imports) {
    *indirect_calls = g_indirect;
    *imports = g_imports;
    g_indirect = g_imports = 0;
}

}  // namespace recomp
