#pragma once
// The runtime: guest memory + the loaded game library + import traps + HLE handlers
// + guest threads. HLE modules register handlers by import name; unregistered
// imports go to a stub that logs `STUB: name(...)` loudly.
#include <cstdint>
#include <filesystem>
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
};

class Runtime {
public:
    bool init(const Paths& paths, std::string* err);

    core::GuestMemory& mem() { return mem_; }
    const loader::ElfImage& elf() const { return elf_; }
    hle::ImportTable& imports() { return imports_; }
    const hle::ImportTable& imports() const { return imports_; }
    const Paths& paths() const { return paths_; }
    cpu::Cpu& main_cpu() { return *main_; }

    // Absolute guest address of a function in the game library (Thumb bit kept).
    uint32_t lib_func(const std::string& name) const;

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
