#include "loader/elf_image.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace loader {

namespace {

constexpr int32_t DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5,
                  DT_SYMTAB = 6, DT_REL = 17, DT_RELSZ = 18, DT_JMPREL = 23;
constexpr uint32_t PT_LOAD = 1, PT_DYNAMIC = 2, SHT_SYMTAB = 2;

template <class T>
T rd(const std::vector<uint8_t>& f, size_t off) {
    T v{};
    if (off + sizeof(T) <= f.size()) std::memcpy(&v, f.data() + off, sizeof(T));
    return v;
}

}  // namespace

bool ElfImage::open(const std::string& path, std::string* err) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        *err = "cannot open " + path;
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    file_.resize(static_cast<size_t>(std::ftell(fp)));
    std::fseek(fp, 0, SEEK_SET);
    size_t got = std::fread(file_.data(), 1, file_.size(), fp);
    std::fclose(fp);
    if (got != file_.size() || file_.size() < 52 || std::memcmp(file_.data(), "\x7f" "ELF", 4) != 0) {
        *err = "not an ELF file";
        return false;
    }
    if (file_[4] != 1 || file_[5] != 1 || rd<uint16_t>(file_, 18) != 40 || rd<uint16_t>(file_, 16) != 3) {
        *err = "not a little-endian ELF32 ARM shared object";
        return false;
    }

    const uint32_t phoff = rd<uint32_t>(file_, 28);
    const uint16_t phentsize = rd<uint16_t>(file_, 42), phnum = rd<uint16_t>(file_, 44);
    uint32_t dyn_off = 0, dyn_size = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        size_t p = phoff + size_t(i) * phentsize;
        uint32_t type = rd<uint32_t>(file_, p);
        if (type == PT_LOAD) {
            LoadSeg s{rd<uint32_t>(file_, p + 8), rd<uint32_t>(file_, p + 20), rd<uint32_t>(file_, p + 16),
                      rd<uint32_t>(file_, p + 4), rd<uint32_t>(file_, p + 24)};
            loads_.push_back(s);
            image_size_ = std::max(image_size_, (s.vaddr + s.memsz + 0xFFF) & ~0xFFFu);
        } else if (type == PT_DYNAMIC) {
            dyn_off = rd<uint32_t>(file_, p + 4);
            dyn_size = rd<uint32_t>(file_, p + 16);
        }
    }
    if (loads_.empty() || !dyn_off) {
        *err = "no PT_LOAD or PT_DYNAMIC";
        return false;
    }
    for (uint32_t o = dyn_off; o + 8 <= dyn_off + dyn_size; o += 8) {
        int32_t tag = rd<int32_t>(file_, o);
        if (tag == DT_NULL) break;
        dynamic_.push_back({tag, rd<uint32_t>(file_, o + 4)});
    }

    // Dynamic symbols: count = nchain of the SysV hash table.
    const uint32_t strtab = vaddr_to_off(dyn(DT_STRTAB)), symtab = vaddr_to_off(dyn(DT_SYMTAB));
    const uint32_t nsyms = rd<uint32_t>(file_, vaddr_to_off(dyn(DT_HASH)) + 4);
    for (const auto& [tag, val] : dynamic_)
        if (tag == DT_NEEDED) needed_.push_back(str_at(strtab, val));
    dynsyms_.reserve(nsyms);
    for (uint32_t i = 0; i < nsyms; ++i) {
        size_t p = symtab + size_t(i) * 16;
        Sym s;
        s.name = str_at(strtab, rd<uint32_t>(file_, p));
        s.value = rd<uint32_t>(file_, p + 4);
        s.size = rd<uint32_t>(file_, p + 8);
        uint8_t info = rd<uint8_t>(file_, p + 12);
        s.type = info & 0xF;
        s.bind = info >> 4;
        s.shndx = rd<uint16_t>(file_, p + 14);
        dynsyms_.push_back(std::move(s));
    }
    read_rels(dyn(DT_REL), dyn(DT_RELSZ), rel_dyn_);
    read_rels(dyn(DT_JMPREL), dyn(DT_PLTRELSZ), rel_plt_);

    // Optional full symbol table from the section headers (diagnostics only).
    const uint32_t shoff = rd<uint32_t>(file_, 32);
    const uint16_t shentsize = rd<uint16_t>(file_, 46), shnum = rd<uint16_t>(file_, 48);
    for (uint16_t i = 0; shoff && i < shnum; ++i) {
        size_t sh = shoff + size_t(i) * shentsize;
        if (rd<uint32_t>(file_, sh + 4) != SHT_SYMTAB) continue;
        uint32_t off = rd<uint32_t>(file_, sh + 16), size = rd<uint32_t>(file_, sh + 20);
        uint32_t link = rd<uint32_t>(file_, sh + 24);
        uint32_t str_off = rd<uint32_t>(file_, shoff + size_t(link) * shentsize + 16);
        for (uint32_t p = off; p + 16 <= off + size; p += 16) {
            Sym s;
            s.name = str_at(str_off, rd<uint32_t>(file_, p));
            s.value = rd<uint32_t>(file_, p + 4);
            s.size = rd<uint32_t>(file_, p + 8);
            uint8_t info = rd<uint8_t>(file_, p + 12);
            s.type = info & 0xF;
            s.bind = info >> 4;
            s.shndx = rd<uint16_t>(file_, p + 14);
            if (s.shndx != SHN_UNDEF && !s.name.empty() && (s.type == STT_FUNC || s.type == STT_OBJECT))
                symtab_.push_back(std::move(s));
        }
    }
    for (const Sym& s : symtab_) {
        by_name_.emplace(s.name, &s);
        if (s.type == STT_FUNC) funcs_sorted_.push_back(&s);
    }
    std::sort(funcs_sorted_.begin(), funcs_sorted_.end(),
              [](const Sym* a, const Sym* b) { return (a->value & ~1u) < (b->value & ~1u); });
    return true;
}

uint32_t ElfImage::dyn(int32_t tag, uint32_t def) const {
    for (const auto& [t, v] : dynamic_)
        if (t == tag) return v;
    return def;
}

uint32_t ElfImage::vaddr_to_off(uint32_t va) const {
    for (const LoadSeg& s : loads_)
        if (va >= s.vaddr && va < s.vaddr + s.filesz) return s.offset + (va - s.vaddr);
    return 0;
}

void ElfImage::read_rels(uint32_t va, uint32_t size, std::vector<Rel>& out) const {
    if (!va) return;
    uint32_t off = vaddr_to_off(va);
    for (uint32_t i = 0; i + 8 <= size; i += 8) {
        uint32_t info = rd<uint32_t>(file_, off + i + 4);
        out.push_back({rd<uint32_t>(file_, off + i), info & 0xFF, info >> 8});
    }
}

std::string ElfImage::str_at(uint32_t strtab_off, uint32_t idx) const {
    size_t p = size_t(strtab_off) + idx;
    if (p >= file_.size()) return {};
    const char* s = reinterpret_cast<const char*>(file_.data() + p);
    return std::string(s, strnlen(s, file_.size() - p));
}

bool ElfImage::relocate_into(core::GuestMemory& mem, uint32_t base, const ImportResolver& resolve,
                             RelocStats* st, std::string* err) const {
    if (!mem.is_mapped(base, image_size_)) {
        // Regions may be split per segment; check every segment instead.
        for (const LoadSeg& s : loads_)
            if (!mem.is_mapped(base + s.vaddr, s.memsz)) {
                *err = "load segments are not mapped";
                return false;
            }
    }
    for (const LoadSeg& s : loads_) {
        mem.write(base + s.vaddr, file_.data() + s.offset, s.filesz);
        if (s.memsz > s.filesz) std::memset(mem.ptr(base + s.vaddr + s.filesz), 0, s.memsz - s.filesz);
    }
    std::unordered_map<uint32_t, uint32_t> resolved;  // dynsym index -> address
    auto symval = [&](uint32_t idx) -> uint32_t {
        const Sym& s = dynsyms_[idx];
        if (!s.undefined()) return base + s.value;
        auto it = resolved.find(idx);
        if (it != resolved.end()) return it->second;
        uint32_t a = resolve(s);
        resolved.emplace(idx, a);
        return a;
    };
    RelocStats stats;
    for (const std::vector<Rel>* table : {&rel_dyn_, &rel_plt_}) {
        for (const Rel& r : *table) {
            uint32_t where = base + r.offset;
            uint32_t cur = mem.r32(where);
            switch (r.type) {
                case R_ARM_RELATIVE: mem.w32(where, cur + base); ++stats.relative; break;
                case R_ARM_ABS32: mem.w32(where, cur + symval(r.sym)); ++stats.abs32; break;
                case R_ARM_GLOB_DAT: mem.w32(where, symval(r.sym)); ++stats.glob_dat; break;
                case R_ARM_JUMP_SLOT: mem.w32(where, symval(r.sym)); ++stats.jump_slot; break;
                default:
                    char buf[96];
                    std::snprintf(buf, sizeof buf, "unsupported relocation type %u at %#x", r.type, r.offset);
                    *err = buf;
                    return false;
            }
        }
    }
    if (st) *st = stats;
    return true;
}

uint32_t ElfImage::func(const std::string& name) const {
    auto it = by_name_.find(name);
    if (it != by_name_.end() && it->second->type == STT_FUNC) return it->second->value;
    for (const Sym& s : dynsyms_)
        if (!s.undefined() && s.type == STT_FUNC && s.name == name) return s.value;
    return 0;
}

const Sym* ElfImage::object(const std::string& name) const {
    auto it = by_name_.find(name);
    return it != by_name_.end() && it->second->type == STT_OBJECT ? it->second : nullptr;
}

const Sym* ElfImage::func_starting_at(uint32_t rel) const {
    auto it = std::lower_bound(funcs_sorted_.begin(), funcs_sorted_.end(), rel,
                               [](const Sym* s, uint32_t v) { return (s->value & ~1u) < v; });
    return it != funcs_sorted_.end() && ((*it)->value & ~1u) == rel ? *it : nullptr;
}

std::string ElfImage::sym_at(uint32_t rel) const {
    rel &= ~1u;
    auto it = std::upper_bound(funcs_sorted_.begin(), funcs_sorted_.end(), rel,
                               [](uint32_t v, const Sym* s) { return v < (s->value & ~1u); });
    char buf[64];
    if (it == funcs_sorted_.begin()) {
        std::snprintf(buf, sizeof buf, "lib+%#x", rel);
        return buf;
    }
    const Sym* s = *(it - 1);
    uint32_t off = rel - (s->value & ~1u);
    std::snprintf(buf, sizeof buf, "+%#x", off);
    if (s->size && off >= s->size) {
        std::snprintf(buf, sizeof buf, "lib+%#x (after ", rel);
        return buf + s->name + ")";
    }
    return s->name + buf;
}

}  // namespace loader
