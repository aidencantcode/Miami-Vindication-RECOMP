#include "core/guest_memory.h"

#include <windows.h>

#include "core/log.h"

namespace core {

static constexpr uint64_t kSpace = 1ull << 32;
static constexpr uint32_t kPage = 0x1000;

GuestMemory::~GuestMemory() {
    if (base_) VirtualFree(base_, 0, MEM_RELEASE);
}

bool GuestMemory::init() {
    base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSpace, MEM_RESERVE, PAGE_NOACCESS));
    if (!base_) {
        LOG("GuestMemory: could not reserve 4 GB of address space (error %lu)", GetLastError());
        return false;
    }
    return true;
}

bool GuestMemory::map(uint32_t addr, uint32_t size, uint32_t prot, const char* name) {
    if ((addr | size) & (kPage - 1) || size == 0 || uint64_t(addr) + size > kSpace) {
        LOG("GuestMemory::map(%08x, %x, %s): not page aligned / out of range", addr, size, name);
        return false;
    }
    for (const Region& r : regions_) {
        if (addr < r.addr + r.size && r.addr < addr + size) {
            LOG("GuestMemory::map(%08x, %x, %s): overlaps %s", addr, size, name, r.name.c_str());
            return false;
        }
    }
    if (!VirtualAlloc(base_ + addr, size, MEM_COMMIT, PAGE_READWRITE)) {
        LOG("GuestMemory::map(%08x, %x, %s): commit failed (error %lu)", addr, size, name, GetLastError());
        return false;
    }
    regions_.push_back({addr, size, prot, name});
    for (auto& l : listeners_)
        if (l.on_map) l.on_map(regions_.back());
    return true;
}

void GuestMemory::protect(uint32_t addr, uint32_t size, uint32_t prot) {
    for (auto& l : listeners_)
        if (l.on_protect) l.on_protect(addr, size, prot);
}

bool GuestMemory::host_writable(uint32_t addr, uint32_t size) {
    const uint32_t a = addr & ~(kPage - 1), e = (addr + size + kPage - 1) & ~(kPage - 1);
    DWORD old = 0;
    return VirtualProtect(base_ + a, e - a, PAGE_READWRITE, &old) != 0;
}

const Region* GuestMemory::region_of(uint32_t addr) const {
    for (const Region& r : regions_)
        if (addr >= r.addr && addr - r.addr < r.size) return &r;
    return nullptr;
}

bool GuestMemory::is_mapped(uint32_t addr, uint32_t size) const {
    const Region* r = region_of(addr);
    return r && uint64_t(addr) + size <= uint64_t(r->addr) + r->size;
}

void GuestMemory::add_listener(Listener l) {
    listeners_.push_back(std::move(l));
    for (const Region& r : regions_)
        if (listeners_.back().on_map) listeners_.back().on_map(r);
}

uint32_t BumpAllocator::alloc(uint32_t size, uint32_t align) {
    uint32_t a = (next_ + align - 1) & ~(align - 1);
    if (uint64_t(a) + (size ? size : 1) > end_) FATAL("BumpAllocator: out of space (%u bytes requested)", size);
    next_ = a + (size ? size : 1);
    return a;
}

}  // namespace core
