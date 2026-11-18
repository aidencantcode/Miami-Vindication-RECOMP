"""Summarise a 32-bit ARM Android .so: header, ARM build attributes, sections,
dynamic section, relocations, imports and exports.

Usage:  python tools/elf_report.py <file.so> [--json out.json]
"""
import argparse
import collections
import json
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.dynamic import DynamicSection
from elftools.elf.relocation import RelocationSection
from elftools.elf.sections import SymbolTableSection

# e_flags bits for ARM (see ELF for the ARM Architecture, "e_flags")
EF_ARM_EABIMASK = 0xFF000000
EF_ARM_ABI_FLOAT_SOFT = 0x00000200
EF_ARM_ABI_FLOAT_HARD = 0x00000400

ARM_RELOC_NAMES = {
    2: "R_ARM_ABS32", 3: "R_ARM_REL32", 20: "R_ARM_COPY", 21: "R_ARM_GLOB_DAT",
    22: "R_ARM_JUMP_SLOT", 23: "R_ARM_RELATIVE", 17: "R_ARM_TLS_DTPMOD32",
    18: "R_ARM_TLS_DTPOFF32", 19: "R_ARM_TLS_TPOFF32",
}

# Tag numbers from the ARM "Addenda to, and Errata in, the ABI for the ARM Architecture"
ATTR_NAMES = {
    4: "Tag_CPU_raw_name", 5: "Tag_CPU_name", 6: "Tag_CPU_arch", 7: "Tag_CPU_arch_profile",
    8: "Tag_ARM_ISA_use", 9: "Tag_THUMB_ISA_use", 10: "Tag_FP_arch", 11: "Tag_WMMX_arch",
    12: "Tag_Advanced_SIMD_arch", 18: "Tag_ABI_PCS_wchar_t", 20: "Tag_ABI_FP_denormal",
    21: "Tag_ABI_FP_exceptions", 23: "Tag_ABI_FP_number_model", 24: "Tag_ABI_align_needed",
    25: "Tag_ABI_align_preserved", 26: "Tag_ABI_enum_size", 28: "Tag_ABI_VFP_args",
    30: "Tag_ABI_optimization_goals", 34: "Tag_CPU_unaligned_access", 38: "Tag_ABI_FP_16bit_format",
    44: "Tag_DIV_use", 68: "Tag_Virtualization_use",
}
CPU_ARCH = {0: "Pre-v4", 1: "v4", 2: "v4T", 3: "v5T", 4: "v5TE", 5: "v5TEJ", 6: "v6",
            7: "v6KZ", 8: "v6T2", 9: "v6K", 10: "v7", 11: "v6-M", 12: "v6S-M", 13: "v7E-M"}
FP_ARCH = {0: "none", 1: "VFPv1", 2: "VFPv2", 3: "VFPv3", 4: "VFPv3-D16", 5: "VFPv4",
           6: "VFPv4-D16"}
VFP_ARGS = {0: "base (soft-float calling convention)", 1: "VFP registers (hard-float)",
            2: "toolchain-specific", 3: "compatible with both"}


def arm_attributes(elf):
    sec = elf.get_section_by_name(".ARM.attributes")
    if sec is None:
        return None
    out = {}
    try:
        for subsec in sec.iter_subsections():
            for subsubsec in subsec.iter_subsubsections():
                for attr in subsubsec.iter_attributes():
                    name = ATTR_NAMES.get(attr.tag if isinstance(attr.tag, int) else -1, str(attr.tag))
                    val = attr.value
                    if name == "Tag_CPU_arch":
                        val = f"{val} ({CPU_ARCH.get(val, '?')})"
                    elif name == "Tag_FP_arch":
                        val = f"{val} ({FP_ARCH.get(val, '?')})"
                    elif name == "Tag_ABI_VFP_args":
                        val = f"{val} ({VFP_ARGS.get(val, '?')})"
                    out[name] = val
    except Exception as exc:  # attribute parsing is best-effort
        out["_error"] = repr(exc)
    return out


def analyse(path):
    f = open(path, "rb")
    elf = ELFFile(f)
    h = elf.header
    rep = {"file": path}
    flags = h["e_flags"]
    rep["header"] = {
        "class": elf.elfclass, "endian": "little" if elf.little_endian else "big",
        "type": h["e_type"], "machine": h["e_machine"], "entry": hex(h["e_entry"]),
        "e_flags": hex(flags), "eabi_version": (flags & EF_ARM_EABIMASK) >> 24,
        "float_abi_flag": "hard" if flags & EF_ARM_ABI_FLOAT_HARD else
                          "soft" if flags & EF_ARM_ABI_FLOAT_SOFT else "unspecified",
    }
    rep["arm_attributes"] = arm_attributes(elf)

    rep["segments"] = [
        {"type": s["p_type"], "vaddr": hex(s["p_vaddr"]), "memsz": s["p_memsz"],
         "filesz": s["p_filesz"], "flags": "".join(c for c, b in (("R", 4), ("W", 2), ("X", 1)) if s["p_flags"] & b),
         "align": hex(s["p_align"])}
        for s in elf.iter_segments()
    ]
    rep["sections"] = [
        {"name": s.name, "type": s["sh_type"], "addr": hex(s["sh_addr"]), "size": s["sh_size"]}
        for s in elf.iter_sections()
    ]
    names = {s.name for s in elf.iter_sections()}
    symtab = elf.get_section_by_name(".symtab")
    rep["symbols"] = {
        "has_symtab": symtab is not None,
        "symtab_count": symtab.num_symbols() if symtab else 0,
        "has_debug_info": any(n.startswith(".debug") for n in names),
        "debug_sections": sorted(n for n in names if n.startswith(".debug")),
    }

    dyn = {"needed": [], "soname": None, "other": {}}
    for sec in elf.iter_sections():
        if isinstance(sec, DynamicSection):
            for tag in sec.iter_tags():
                if tag.entry.d_tag == "DT_NEEDED":
                    dyn["needed"].append(tag.needed)
                elif tag.entry.d_tag == "DT_SONAME":
                    dyn["soname"] = tag.soname
                else:
                    dyn["other"][str(tag.entry.d_tag)] = hex(tag.entry.d_val)
    rep["dynamic"] = dyn

    relocs = collections.Counter()
    for sec in elf.iter_sections():
        if isinstance(sec, RelocationSection):
            for r in sec.iter_relocations():
                relocs[f"{sec.name}:{ARM_RELOC_NAMES.get(r['r_info_type'], r['r_info_type'])}"] += 1
    rep["relocations"] = dict(relocs)

    dynsym = elf.get_section_by_name(".dynsym")
    imports, exports = [], []
    for sym in dynsym.iter_symbols():
        if not sym.name:
            continue
        entry = {"name": sym.name, "type": sym["st_info"]["type"], "bind": sym["st_info"]["bind"]}
        if sym["st_shndx"] == "SHN_UNDEF":
            imports.append(entry)
        else:
            entry["value"] = hex(sym["st_value"])
            entry["size"] = sym["st_size"]
            exports.append(entry)
    rep["imports"] = imports
    rep["exports"] = exports
    return rep


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("so")
    ap.add_argument("--json")
    a = ap.parse_args()
    rep = analyse(a.so)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(rep, fh, indent=1)
    s = rep
    print(f"== {a.so}")
    print("header:", s["header"])
    print("arm_attributes:", s["arm_attributes"])
    print("symbols:", s["symbols"])
    print("needed:", s["dynamic"]["needed"], "soname:", s["dynamic"]["soname"])
    print("dynamic other:", s["dynamic"]["other"])
    print("relocations:", s["relocations"])
    print(f"imports: {len(s['imports'])}  exports: {len(s['exports'])}")
    print("segments:")
    for seg in s["segments"]:
        print("  ", seg)
    print("sections:")
    for sec in s["sections"]:
        print(f"   {sec['name']:<24} {sec['type']:<18} {sec['addr']:>10} {sec['size']:>10}")


if __name__ == "__main__":
    sys.exit(main())
