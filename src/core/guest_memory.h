#pragma once
// The guest's 32-bit address space ("fastmem"): one 4 GB host reservation, so guest
// address A lives at host address base()+A. CPU backends (Unicorn now, recompiled
// code later) map the same host pages; HLE code turns guest pointers into host
// pointers with one addition.
//
// Host-side protection is always read/write (HLE needs to write anywhere it is
// told to); guest-visible protection is enforced by the CPU backend.
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace core {

enum : uint32_t { PROT_R = 1, PROT_W = 2, PROT_X = 4 };

struct Region {
    uint32_t addr;
    uint32_t size;
    uint32_t prot;
    std::string name;
};

class GuestMemory {
public:
    GuestMemory() = default;
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;
    ~GuestMemory();

    bool init();  // reserve 4 GB of host address space

    // Commit [addr, addr+size) (4 KB aligned, must not overlap an existing region).
    bool map(uint32_t addr, uint32_t size, uint32_t prot, const char* name);
    // Change guest-visible protection of part of a region (backends are notified).
    void protect(uint32_t addr, uint32_t size, uint32_t prot);
    // Make the host pages under [addr, addr+size) read/write again (the recompiled backend makes
    // read-only guest regions read-only on the host). For words the port patches in game code.
    bool host_writable(uint32_t addr, uint32_t size);

    const std::vector<Region>& regions() const { return regions_; }
    const Region* region_of(uint32_t addr) const;
    bool is_mapped(uint32_t addr, uint32_t size) const;

    uint8_t* base() const { return base_; }
    template <class T = uint8_t>
    T* ptr(uint32_t a) const { return reinterpret_cast<T*>(base_ + a); }
    const char* cstr(uint32_t a) const { return reinterpret_cast<const char*>(base_ + a); }

    uint32_t r32(uint32_t a) const { uint32_t v; std::memcpy(&v, base_ + a, 4); return v; }
    uint16_t r16(uint32_t a) const { uint16_t v; std::memcpy(&v, base_ + a, 2); return v; }
    uint8_t r8(uint32_t a) const { return base_[a]; }
    void w32(uint32_t a, uint32_t v) { std::memcpy(base_ + a, &v, 4); }
    void w16(uint32_t a, uint16_t v) { std::memcpy(base_ + a, &v, 2); }
    void w8(uint32_t a, uint8_t v) { base_[a] = v; }
    void write(uint32_t a, const void* src, size_t n) { std::memcpy(base_ + a, src, n); }
    void read(uint32_t a, void* dst, size_t n) const { std::memcpy(dst, base_ + a, n); }

    struct Listener {
        std::function<void(const Region&)> on_map;
        std::function<void(uint32_t addr, uint32_t size, uint32_t prot)> on_protect;
    };
    // Backends attach here; they are told about all existing regions immediately.
    void add_listener(Listener l);

private:
    uint8_t* base_ = nullptr;
    std::vector<Region> regions_;
    std::vector<Listener> listeners_;
};

// Simple bump allocator over a mapped region (used for HLE-owned allocations).
class BumpAllocator {
public:
    void init(uint32_t begin, uint32_t end) { next_ = begin; end_ = end; begin_ = begin; }
    uint32_t alloc(uint32_t size, uint32_t align = 8);
    uint32_t used() const { return next_ - begin_; }

private:
    uint32_t begin_ = 0, next_ = 0, end_ = 0;
};

}  // namespace core
