#pragma once
// Binds the game's imports: every imported function gets an 8-byte ARM trap stub
// (`svc #0 ; bx lr`) in the trap region; the 7 bionic data objects get memory with
// bionic's layout (see bionic_data.cpp). The CPU backend turns an `svc` at a trap
// into a call of the registered HLE handler.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/guest_memory.h"
#include "loader/elf_image.h"

namespace hle {

class ImportTable {
public:
    bool init(core::GuestMemory& mem);  // maps the trap + data regions, builds bionic data

    uint32_t resolve(const loader::Sym& s);  // trap address (functions) or object address (data)
    uint32_t add_trap(const std::string& name);

    int trap_index(uint32_t svc_addr) const;  // -1 if not a trap
    const std::string& trap_name(int idx) const { return traps_[idx]; }
    size_t trap_count() const { return traps_.size(); }
    uint32_t trap_addr(const std::string& name) const;

    bool is_data(const std::string& name) const { return data_.count(name) != 0; }
    uint32_t data(const std::string& name) const;
    core::BumpAllocator& data_alloc() { return data_alloc_; }

    const std::vector<std::string>& unresolved_objects() const { return unresolved_objects_; }

    // Imports implemented as guest code (no trap): name -> guest address.
    void set_fastpath(std::unordered_map<std::string, uint32_t> m) { fastpath_ = std::move(m); }
    bool is_fastpath(const std::string& name) const { return fastpath_.count(name) != 0; }

private:
    std::unordered_map<std::string, uint32_t> fastpath_;
    core::GuestMemory* mem_ = nullptr;
    core::BumpAllocator data_alloc_;
    std::vector<std::string> traps_;
    std::unordered_map<std::string, uint32_t> trap_by_name_;
    std::unordered_map<std::string, uint32_t> data_;
    std::vector<std::string> unresolved_objects_;
};

// Bionic data objects, written into the data region. Returns name -> address
// (plus "<name>#table" entries for the pointer-to-table objects).
std::unordered_map<std::string, uint32_t> build_bionic_data(core::GuestMemory& mem, core::BumpAllocator& alloc);

}  // namespace hle
