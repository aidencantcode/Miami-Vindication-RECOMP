"""Dump libGangster2's full symbol table (functions + objects), demangled, to
analysis/raw/libGangster2.symbols.txt (one per line: kind addr size name), so it can
be searched with grep. Addresses are library-relative; Thumb functions have bit 0 set.

Usage: python -I tools/syms.py [so]
"""
import os
import sys

from elftools.elf.elffile import ELFFile

try:
    from itanium_demangler import parse as demangle_parse
except ImportError:
    demangle_parse = None

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
so = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "work", "apk", "lib", "armeabi", "libGangster2.so")
out = os.path.join(root, "analysis", "raw", "libGangster2.symbols.txt")


def demangle(n):
    if demangle_parse and n.startswith("_Z"):
        try:
            d = demangle_parse(n)
            if d is not None:
                return str(d)
        except Exception:
            pass
    return n


elf = ELFFile(open(so, "rb"))
rows = []
for s in elf.get_section_by_name(".symtab").iter_symbols():
    t = s["st_info"]["type"]
    if s["st_shndx"] == "SHN_UNDEF" or not s.name or t not in ("STT_FUNC", "STT_OBJECT"):
        continue
    rows.append((s["st_value"], "F" if t == "STT_FUNC" else "O", s["st_size"], demangle(s.name), s.name))
rows.sort()
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "w", encoding="utf-8") as fh:
    for v, k, size, name, raw in rows:
        fh.write(f"{k} {v:08x} {size:6d} {name}" + (f"   [{raw}]" if name != raw else "") + "\n")
print(f"{len(rows)} symbols -> {out}")
