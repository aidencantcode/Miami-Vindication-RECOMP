#pragma once
// The runtime: guest memory + the loaded game library + import traps + HLE handlers
// + guest threads. HLE modules register handlers by import name; unregistered
// imports go to a stub that logs `STUB: name(...)` loudly.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/guest_memory.h"
#include "cpu/cpu.h"
#include "hle/imports.h"
#include "loader/elf_image.h"

namespace rt {

struct Paths {
    std::filesystem::path root;       // project root
    std::filesystem::path so;         // libGangster2.so
    std::filesystem::path data;       // game data folder (read-only)
    std::filesystem::path sandbox;    // where guest writes go
    std::filesystem::path user;       // settings.ini etc.
};

class Runtime {
public:
    // fastpath: run the ARM EABI float/division helpers as guest VFP code (src/cpu/fastpath.S)
    // instead of trapping to C++ for every call. Off = the original (slow, simplest) path.
    bool init(const Paths& paths, std::string* err, bool fastpath = true);

    core::GuestMemory& mem() { return mem_; }
    const loader::ElfImage& elf() const { return elf_; }
    hle::ImportTable& imports() { return imports_; }
    const hle::ImportTable& imports() const { return imports_; }
    const Paths& paths() const { return paths_; }
    cpu::Cpu& main_cpu() { return *main_; }

    // Absolute guest address of a function in the game library (Thumb bit kept).
    uint32_t lib_func(const std::string& name) const;

    // ---- function hooks (game functions, by symbol name) ----------------------------------------
    // pre:  runs when the game enters the function; return true to SKIP it (pre sets r0 if the
    //       function returns a value), false to let it run.
    // post: runs when the function returns (r0/r1 hold its result), unless pre skipped it.
    struct FunctionHook {
        std::function<bool(cpu::Cpu&)> pre;
        std::function<void(cpu::Cpu&)> post;
    };
    void hook_function(const std::string& symbol, FunctionHook h);
    void run_function_hook(cpu::Cpu& c, uint32_t addr);  // called by Cpu (code hook)
    // The same hooks for the recompiled backend (generated entry/exit checks, rc_runtime.cpp):
    int hook_index(uint32_t addr) const;          // -1: not hooked
    bool run_pre_hook(cpu::Cpu& c, int idx);      // true: skip the function (r0 set by the hook)
    void run_post_hook(cpu::Cpu& c, int idx);
    // Debugging: write all guest memory (format of GANGSTAR_MEMDUMP_AT) and log the registers.
    void dump_state(cpu::Cpu& c, const std::string& path, const std::string& label);
    uint32_t lib_object(const std::string& symbol) const;  // absolute address of a game global
    // Calls per hooked function so far (name, count), for --profile-traps reports.
    std::vector<std::pair<std::string, uint64_t>> hook_counts() const;

    void set_handler(const std::string& import_name, cpu::Handler h);
    bool has_handler(const std::string& name) const { return by_name_.count(name) != 0; }
    // Register a handler for a trap that is not an ELF import (JNI table slots etc.).
    uint32_t add_trap(const std::string& name, cpu::Handler h);
    void dispatch(cpu::Cpu& c, int trap_index, cpu::Args& a);

    // Statistics for reports
    uint64_t calls(const std::string& trap_name) const;
    const std::vector<int>& first_use_order() const { return first_use_; }
    const std::vector<uint64_t>& call_counts() const { return counts_; }
    std::vector<std::string> stubbed() const;

    // Memory owned by the HLE layer
    core::BumpAllocator& heap() { return heap_; }
    uint32_t sbrk(int32_t incr);  // returns old break or 0xFFFFFFFF
    uint32_t brk() const { return brk_; }
    uint32_t mmap_anon(uint32_t size);

    uint32_t errno_addr() const { return errno_addr_; }

private:
    void ensure_table();
    static void post_hook_trap(cpu::Cpu& c, cpu::Args& a);

    std::vector<FunctionHook> fn_hooks_;
    std::unordered_map<uint32_t, int> fn_hook_at_;  // entry address (no Thumb bit) -> hook index
    std::vector<std::string> fn_hook_names_;
    std::vector<uint64_t> fn_hook_calls_;
    uint32_t post_trap_ = 0;

    Paths paths_;
    core::GuestMemory mem_;
    loader::ElfImage elf_;
    hle::ImportTable imports_;
    std::unique_ptr<cpu::Cpu> main_;
    std::vector<cpu::Handler> table_;
    std::unordered_map<std::string, cpu::Handler> by_name_;
    std::vector<uint64_t> counts_;
    std::vector<int> first_use_;
    std::vector<bool> stubbed_;
    core::BumpAllocator heap_;
    uint32_t brk_ = 0, mmap_next_ = 0, errno_addr_ = 0;
};

}  // namespace rt
