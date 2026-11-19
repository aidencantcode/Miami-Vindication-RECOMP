"""What the recompiler needs from libGangster2.so: bytes by virtual address, functions, mapping
symbols (code/data map), the PLT (entry address -> import name) and relocations."""
import bisect
import struct
from dataclasses import dataclass

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection


@dataclass
class Func:
    name: str
    addr: int      # without the Thumb bit
    size: int
    thumb: bool


class Elf:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.raw = f.read()
        import io
        self.elf = ELFFile(io.BytesIO(self.raw))
        self.segs = [(s["p_vaddr"], s["p_filesz"], s["p_offset"]) for s in self.elf.iter_segments()
                     if s["p_type"] == "PT_LOAD"]
        self.sections = {s.name: (s["sh_addr"], s["sh_size"]) for s in self.elf.iter_sections()}
        self._symbols()
        self._plt()

    # ---- bytes -----------------------------------------------------------------------------------
    def off(self, va):
        for v, n, o in self.segs:
            if v <= va < v + n:
                return o + (va - v)
        raise ValueError(f"address {va:#x} not in a file-backed segment")

    def u16(self, va):
        return struct.unpack_from("<H", self.raw, self.off(va))[0]

    def u32(self, va):
        return struct.unpack_from("<I", self.raw, self.off(va))[0]

    def section(self, name):
        return self.sections.get(name, (0, 0))

    # ---- symbols ---------------------------------------------------------------------------------
    def _symbols(self):
        st = self.elf.get_section_by_name(".symtab")
        self.funcs = {}       # addr -> Func (first name wins; aliases in self.aliases)
        self.aliases = {}     # addr -> [names]
        maps = []
        for s in st.iter_symbols():
            name, val, typ = s.name, s["st_value"], s["st_info"]["type"]
            if name in ("$a", "$t", "$d") or name.startswith(("$a.", "$t.", "$d.")):
                maps.append((val, name[1]))
                continue
            if typ == "STT_FUNC" and s["st_shndx"] != "SHN_UNDEF" and val:
                addr, thumb = val & ~1, bool(val & 1)
                self.aliases.setdefault(addr, []).append(name)
                if addr not in self.funcs:
                    self.funcs[addr] = Func(name, addr, s["st_size"], thumb)
                elif s["st_size"] > self.funcs[addr].size:
                    self.funcs[addr].size = s["st_size"]
        maps.sort()
        # collapse duplicates at the same address (last one wins: same kind in practice)
        self.map_addrs, self.map_kinds = [], []
        for a, k in maps:
            if self.map_addrs and self.map_addrs[-1] == a:
                self.map_kinds[-1] = k
            else:
                self.map_addrs.append(a)
                self.map_kinds.append(k)
        self.func_addrs = sorted(self.funcs)
        self.got_base = next((s["st_value"] for s in st.iter_symbols() if s.name == "_GLOBAL_OFFSET_TABLE_"), 0)

    def kind_at(self, va):
        """'t', 'a', 'd' or None (before any mapping symbol)."""
        i = bisect.bisect_right(self.map_addrs, va) - 1
        return self.map_kinds[i] if i >= 0 else None

    def regions(self, lo, hi):
        """[(start, end, kind)] covering [lo, hi) by mapping symbols."""
        i = max(0, bisect.bisect_right(self.map_addrs, lo) - 1)
        out = []
        while i < len(self.map_addrs) and self.map_addrs[i] < hi:
            s = max(lo, self.map_addrs[i])
            e = min(hi, self.map_addrs[i + 1] if i + 1 < len(self.map_addrs) else hi)
            if e > s:
                out.append((s, e, self.map_kinds[i]))
            i += 1
        return out

    def func_containing(self, va):
        i = bisect.bisect_right(self.func_addrs, va) - 1
        if i < 0:
            return None
        f = self.funcs[self.func_addrs[i]]
        return f if va < f.addr + max(f.size, 2) else None

    # ---- PLT / imports -------------------------------------------------------------------------
    def _plt(self):
        """plt entry address -> import name, by decoding each ARM PLT stub's GOT slot."""
        got_name = {}
        for sec in self.elf.iter_sections():
            if isinstance(sec, RelocationSection) and sec.name in (".rel.plt", ".rel.dyn"):
                syms = self.elf.get_section(sec["sh_link"])
                for r in sec.iter_relocations():
                    if r["r_info_sym"]:
                        got_name[r["r_offset"]] = syms.get_symbol(r["r_info_sym"]).name
        self.got_name = got_name
        self.plt = {}
        lo, size = self.section(".plt")

        def imm(w):
            v, r = w & 0xFF, ((w >> 8) & 0xF) * 2
            return ((v >> r) | (v << (32 - r))) & 0xFFFFFFFF if r else v

        a = lo
        while a + 12 <= lo + size:
            w0, w1, w2 = self.u32(a), self.u32(a + 4), self.u32(a + 8)
            if (w0 & 0xFFFFF000) == 0xE28FC000 and (w1 & 0xFFFFF000) == 0xE28CC000 and (w2 & 0xFFFFF000) == 0xE5BCF000:
                got = (a + 8 + imm(w0) + imm(w1) + (w2 & 0xFFF)) & 0xFFFFFFFF
                self.plt[a] = got_name.get(got, f"<got {got:#x}>")
                a += 12
            else:
                a += 4
