#pragma once
// ELF32/ARM shared-object loader with Android 2.x linker relocation semantics
// (REL format):
//   R_ARM_RELATIVE           *where += base
//   R_ARM_ABS32              *where += S
//   R_ARM_GLOB_DAT/JUMP_SLOT *where  = S
// S = base + st_value for symbols the library defines (DT_SYMBOLIC: own symbols win),
// otherwise whatever the import resolver returns. st_value keeps the Thumb bit.
//
// Everything needed for loading is read from the dynamic section (as the real linker
// does); the section-header symbol table (.symtab, present in this unstripped build)
// is only used for diagnostics.
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/guest_memory.h"

namespace loader {

enum : uint32_t {
    R_ARM_ABS32 = 2, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23,
    STT_OBJECT = 1, STT_FUNC = 2, SHN_UNDEF = 0,
};

struct Sym {
    std::string name;
    uint32_t value = 0, size = 0;
    uint8_t type = 0, bind = 0;
    uint16_t shndx = 0;
    bool undefined() const { return shndx == SHN_UNDEF; }
};

struct Rel {
    uint32_t offset, type, sym;
};

struct LoadSeg {
    uint32_t vaddr, memsz, filesz, offset, flags;  // flags: PF_X=1, PF_W=2, PF_R=4
};

struct RelocStats {
    uint32_t relative = 0, abs32 = 0, glob_dat = 0, jump_slot = 0;
};

class ElfImage {
public:
    bool open(const std::string& path, std::string* err);

    uint32_t image_size() const { return image_size_; }
    const std::vector<LoadSeg>& loads() const { return loads_; }
    uint32_t dyn(int32_t tag, uint32_t def = 0) const;
    const std::vector<std::string>& needed() const { return needed_; }
    const std::vector<Sym>& dynsyms() const { return dynsyms_; }
    const std::vector<Rel>& rel_dyn() const { return rel_dyn_; }
    const std::vector<Rel>& rel_plt() const { return rel_plt_; }
    const std::vector<Sym>& symtab() const { return symtab_; }

    // Copy the segments into guest memory at `base` (regions must already be mapped)
    // and apply all relocations.
    using ImportResolver = std::function<uint32_t(const Sym&)>;
    bool relocate_into(core::GuestMemory& mem, uint32_t base, const ImportResolver& resolve,
                       RelocStats* stats, std::string* err) const;

    // Diagnostics (relative addresses, i.e. without the load base)
    uint32_t func(const std::string& name) const;  // st_value incl. Thumb bit; 0 if absent
    std::string sym_at(uint32_t rel) const;         // "name+0x12"
    const Sym* func_starting_at(uint32_t rel_no_thumb) const;
    const Sym* object(const std::string& name) const;

private:
    uint32_t vaddr_to_off(uint32_t va) const;
    void read_rels(uint32_t va, uint32_t size, std::vector<Rel>& out) const;
    std::string str_at(uint32_t strtab_off, uint32_t idx) const;

    std::vector<uint8_t> file_;
    std::vector<LoadSeg> loads_;
    uint32_t image_size_ = 0;
    std::vector<std::pair<int32_t, uint32_t>> dynamic_;
    std::vector<std::string> needed_;
    std::vector<Sym> dynsyms_;
    std::vector<Rel> rel_dyn_, rel_plt_;
    std::vector<Sym> symtab_;
    std::vector<const Sym*> funcs_sorted_;                 // by value & ~1
    std::unordered_map<std::string, const Sym*> by_name_;  // FUNC + OBJECT from .symtab
};

}  // namespace loader
