#include "hle/imports.h"

#include "core/layout.h"
#include "core/log.h"

namespace hle {

static constexpr uint32_t kSvc0 = 0xEF000000, kBxLr = 0xE12FFF1E;

bool ImportTable::init(core::GuestMemory& mem) {
    mem_ = &mem;
    if (!mem.map(layout::kTrapBase, layout::kTrapSize, core::PROT_R | core::PROT_X, "traps")) return false;
    if (!mem.map(layout::kDataBase, layout::kDataSize, core::PROT_R | core::PROT_W, "bionic data")) return false;
    data_alloc_.init(layout::kDataBase, layout::kDataBase + layout::kDataSize);
    // Trap 0 is the "return to host" sentinel, as in the probe, so import trap
    // addresses are identical in both implementations.
    add_trap("<return-to-host>");
    data_ = build_bionic_data(mem, data_alloc_);
    return true;
}

uint32_t ImportTable::add_trap(const std::string& name) {
    auto it = trap_by_name_.find(name);
    if (it != trap_by_name_.end()) return it->second;
    uint32_t addr = layout::kTrapBase + 8 * uint32_t(traps_.size());
    if (addr + 8 > layout::kTrapBase + layout::kTrapSize) FATAL("trap region full");
    mem_->w32(addr, kSvc0);
    mem_->w32(addr + 4, kBxLr);
    traps_.push_back(name);
    trap_by_name_.emplace(name, addr);
    return addr;
}

uint32_t ImportTable::resolve(const loader::Sym& s) {
    if (auto it = data_.find(s.name); it != data_.end()) return it->second;
    if (auto it = fastpath_.find(s.name); it != fastpath_.end()) return it->second;
    if (s.type == loader::STT_OBJECT) {
        // Unknown data import: give it zeroed memory but remember it (reported as a failure).
        unresolved_objects_.push_back(s.name);
        uint32_t a = data_alloc_.alloc(64, 8);
        data_.emplace(s.name, a);
        return a;
    }
    return add_trap(s.name);
}

int ImportTable::trap_index(uint32_t svc_addr) const {
    if (svc_addr < layout::kTrapBase || svc_addr >= layout::kTrapBase + 8 * traps_.size()) return -1;
    if ((svc_addr - layout::kTrapBase) & 7) return -1;
    return int((svc_addr - layout::kTrapBase) / 8);
}

uint32_t ImportTable::trap_addr(const std::string& name) const {
    auto it = trap_by_name_.find(name);
    return it == trap_by_name_.end() ? 0 : it->second;
}

uint32_t ImportTable::data(const std::string& name) const {
    auto it = data_.find(name);
    return it == data_.end() ? 0 : it->second;
}

}  // namespace hle
