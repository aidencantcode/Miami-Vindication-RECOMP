"""Dump printable C strings from an ELF's data sections with their virtual
addresses (like `strings -t x`, but in VA space so they match Ghidra).

Usage: python tools/so_strings.py <file.so> <out.txt> [--min 4]
"""
import argparse
import re

from elftools.elf.elffile import ELFFile

SECTIONS = (".rodata", ".data", ".data.rel.ro")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("so")
    ap.add_argument("out")
    ap.add_argument("--min", type=int, default=4)
    a = ap.parse_args()
    elf = ELFFile(open(a.so, "rb"))
    pat = re.compile(rb"[\x20-\x7e\t]{%d,}" % a.min)
    n = 0
    with open(a.out, "w", encoding="utf-8") as out:
        for name in SECTIONS:
            sec = elf.get_section_by_name(name)
            if sec is None:
                continue
            data, base = sec.data(), sec["sh_addr"]
            for m in pat.finditer(data):
                out.write(f"{base + m.start():08x} {name} {m.group().decode('ascii')}\n")
                n += 1
    print(f"{n} strings -> {a.out}")


if __name__ == "__main__":
    main()
