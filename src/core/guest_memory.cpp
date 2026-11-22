#include "core/guest_memory.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "core/log.h"

namespace core {

static constexpr uint64_t kSpace = 1ull << 32;
static constexpr uint32_t kPage = 0x1000;

// The 4 GB guest space is reserved inaccessible and committed read/write region by region
// (Windows: VirtualAlloc reserve / commit; POSIX: mmap PROT_NONE, then mprotect).
namespace {
#ifdef _WIN32
uint8_t* reserve_space() { return static_cast<uint8_t*>(VirtualAlloc(nullptr, kSpace, MEM_RESERVE, PAGE_NOACCESS)); }
void release_space(uint8_t* p) { VirtualFree(p, 0, MEM_RELEASE); }
bool commit(uint8_t* p, uint32_t size) { return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != nullptr; }
bool make_rw(uint8_t* p, uint32_t size) {
    DWORD old = 0;
    return VirtualProtect(p, size, PAGE_READWRITE, &old) != 0;
}
unsigned long last_error() { return GetLastError(); }
#else
uint8_t* reserve_space() {
    void* p = mmap(nullptr, kSpace, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
}
void release_space(uint8_t* p) { munmap(p, kSpace); }
// Rounded out to the system page size (16 KB on some newer Android devices; guest regions are
// 4 KB aligned, and neighbouring regions sharing a system page are all read/write anyway).
bool protect_rw(uint8_t* p, uint32_t size) {
    static const uintptr_t ps = uintptr_t(sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096);
    const uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(ps - 1);
    const uintptr_t e = (reinterpret_cast<uintptr_t>(p) + size + ps - 1) & ~(ps - 1);
    return mprotect(reinterpret_cast<void*>(a), e - a, PROT_READ | PROT_WRITE) == 0;
}
bool commit(uint8_t* p, uint32_t size) { return protect_rw(p, size); }
bool make_rw(uint8_t* p, uint32_t size) { return protect_rw(p, size); }
unsigned long last_error() { return unsigned(errno); }
#endif
}  // namespace

GuestMemory::~GuestMemory() {
    if (base_) release_space(base_);
}

bool GuestMemory::init() {
    base_ = reserve_space();
    if (!base_) {
        LOG("GuestMemory: could not reserve 4 GB of address space (error %lu)", last_error());
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
    if (!commit(base_ + addr, size)) {
        LOG("GuestMemory::map(%08x, %x, %s): commit failed (error %lu)", addr, size, name, last_error());
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
    return make_rw(base_ + a, e - a);
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
