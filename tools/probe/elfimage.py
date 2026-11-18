"""Load an ARM Android .so into a flat byte image and apply its relocations the way
the old Android linker did (Android 2.x linker.c semantics, REL format):

  R_ARM_RELATIVE          *where += load_base
  R_ARM_ABS32             *where += S
  R_ARM_GLOB_DAT/JUMP_SLOT *where  = S

S = load_base + st_value for symbols the library defines (DT_SYMBOLIC: the
library's own definitions win), or whatever the import resolver returns for
undefined symbols. st_value keeps the Thumb bit, as the real linker does.
"""
import bisect
import collections
import struct

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

R_ARM_ABS32, R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT, R_ARM_RELATIVE = 2, 21, 22, 23
RELOC_NAMES = {R_ARM_ABS32: "R_ARM_ABS32", R_ARM_GLOB_DAT: "R_ARM_GLOB_DAT",
               R_ARM_JUMP_SLOT: "R_ARM_JUMP_SLOT", R_ARM_RELATIVE: "R_ARM_RELATIVE"}
PAGE = 0x1000

try:
    from itanium_demangler import parse as _demangle_parse
except ImportError:  # optional nicety
    _demangle_parse = None


def demangle(name):
    if _demangle_parse and name.startswith("_Z"):
        try:
            d = _demangle_parse(name)
            if d is not None:
                return str(d)
        except Exception:
            pass
    return name


class ElfImage:
    def __init__(self, path, base):
        self.path = path
        self.base = base
        self._fh = open(path, "rb")
        self.elf = ELFFile(self._fh)

        loads = [s for s in self.elf.iter_segments() if s["p_type"] == "PT_LOAD"]
        end = max(s["p_vaddr"] + s["p_memsz"] for s in loads)
        self.size = (end + PAGE - 1) & ~(PAGE - 1)
        self.image = bytearray(self.size)
        self.segments = []  # (vaddr, memsz, flags 'RWX')
        for s in loads:
            data = s.data()
            self.image[s["p_vaddr"]:s["p_vaddr"] + len(data)] = data
            flags = "".join(c for c, b in (("R", 4), ("W", 2), ("X", 1)) if s["p_flags"] & b)
            self.segments.append((s["p_vaddr"], s["p_memsz"], flags))

        self.dynsyms = list(self.elf.get_section_by_name(".dynsym").iter_symbols())
        self.imports = [s for s in self.dynsyms if s.name and s["st_shndx"] == "SHN_UNDEF"]
        self.exports = {s.name: s["st_value"] for s in self.dynsyms
                        if s.name and s["st_shndx"] != "SHN_UNDEF"}

        # Full symbol table (this library is not stripped).
        self.funcs = {}        # name -> st_value (with Thumb bit)
        self.objects = {}      # name -> (st_value, st_size)
        starts = {}            # addr (no Thumb bit) -> (name, size)
        symtab = self.elf.get_section_by_name(".symtab")
        if symtab is not None:
            for s in symtab.iter_symbols():
                if s["st_shndx"] == "SHN_UNDEF" or not s.name:
                    continue
                t = s["st_info"]["type"]
                if t == "STT_FUNC":
                    self.funcs.setdefault(s.name, s["st_value"])
                    starts.setdefault(s["st_value"] & ~1, (s.name, s["st_size"]))
                elif t == "STT_OBJECT":
                    self.objects.setdefault(s.name, (s["st_value"], s["st_size"]))
        self._starts = sorted(starts)
        self._start_info = starts

        self.dynamic = {}
        for sec in self.elf.iter_sections():
            if sec["sh_type"] == "SHT_DYNAMIC":
                for tag in sec.iter_tags():
                    self.dynamic.setdefault(tag.entry.d_tag, tag.entry.d_val)

    # -- relocation --------------------------------------------------------
    def relocate(self, resolve_import):
        """Apply all relocations. resolve_import(sym) -> absolute address."""
        stats = collections.Counter()
        resolved = {}
        for sec in self.elf.iter_sections():
            if not isinstance(sec, RelocationSection):
                continue
            for r in sec.iter_relocations():
                off, rtype = r["r_offset"], r["r_info_type"]
                cur = struct.unpack_from("<I", self.image, off)[0]
                if rtype == R_ARM_RELATIVE:
                    val = cur + self.base
                else:
                    sym = self.dynsyms[r["r_info_sym"]]
                    if sym["st_shndx"] == "SHN_UNDEF":
                        if sym.name not in resolved:
                            resolved[sym.name] = resolve_import(sym)
                        S = resolved[sym.name]
                    else:
                        S = self.base + sym["st_value"]
                    if rtype == R_ARM_ABS32:
                        val = cur + S
                    elif rtype in (R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT):
                        val = S
                    else:
                        raise NotImplementedError(f"relocation type {rtype} at {off:#x}")
                struct.pack_into("<I", self.image, off, val & 0xFFFFFFFF)
                stats[RELOC_NAMES.get(rtype, str(rtype))] += 1
        return stats

    # -- helpers -------------------------------------------------------------
    def u32(self, vaddr):
        return struct.unpack_from("<I", self.image, vaddr)[0]

    def init_array(self):
        a, n = self.dynamic.get("DT_INIT_ARRAY"), self.dynamic.get("DT_INIT_ARRAYSZ", 0)
        return [self.u32(a + 4 * i) for i in range(n // 4)] if a else []

    def sym_at(self, addr):
        """'name+0xoff' for an absolute address inside this library, else None."""
        rel = (addr & ~1) - self.base
        if not 0 <= rel < self.size:
            return None
        i = bisect.bisect_right(self._starts, rel) - 1
        if i < 0:
            return f"lib+{rel:#x}"
        start = self._starts[i]
        name, size = self._start_info[start]
        off = rel - start
        if size and off >= size:
            return f"lib+{rel:#x} (after {demangle(name)})"
        return f"{demangle(name)}+{off:#x}"

    def func_start(self, rel_addr_no_thumb):
        return self._start_info.get(rel_addr_no_thumb)
