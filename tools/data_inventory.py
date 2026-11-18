"""Inventory a Gameloft data folder: files per extension, sizes, and the
leading bytes ("magic") of each extension so formats can be identified.
Read-only.

Usage: python tools/data_inventory.py <data dir> <out.md>
"""
import argparse
import collections
import os


def magic(path, n=16):
    with open(path, "rb") as fh:
        b = fh.read(n)
    hx = " ".join(f"{x:02x}" for x in b)
    asc = "".join(chr(x) if 32 <= x < 127 else "." for x in b)
    return hx, asc


def ext_of(name):
    # Gameloft uses compound extensions like .cst.desc.gmap or .about.glyph;
    # group by the last component but keep the full one for reference.
    if "." not in name:
        return "(none)"
    return name.rsplit(".", 1)[1].lower()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("out")
    a = ap.parse_args()
    by_ext = collections.defaultdict(list)
    dirs = set()
    for dp, dn, fn in os.walk(a.root):
        rel = os.path.relpath(dp, a.root)
        if rel != ".":
            dirs.add(rel)
        for f in fn:
            p = os.path.join(dp, f)
            by_ext[ext_of(f)].append((os.path.relpath(p, a.root), os.path.getsize(p)))

    total = sum(s for v in by_ext.values() for _, s in v)
    count = sum(len(v) for v in by_ext.values())
    lines = [f"# Data inventory: `{a.root}`", "",
             f"{count} files, {total:,} bytes, {len(dirs)} subdirectories", ""]
    if dirs:
        lines += ["Subdirectories: " + ", ".join(sorted(dirs)), ""]
    lines += ["| ext | files | total bytes | example | first 16 bytes | ascii |",
              "|---|---:|---:|---|---|---|"]
    for ext, files in sorted(by_ext.items(), key=lambda kv: -sum(s for _, s in kv[1])):
        files.sort()
        ex_name, _ = files[0]
        hx, asc = magic(os.path.join(a.root, ex_name))
        asc = asc.replace("|", "\\|")
        lines.append(f"| {ext} | {len(files)} | {sum(s for _, s in files):,} | `{ex_name}` | `{hx}` | `{asc}` |")

    # Distinct magic prefixes per extension (first 4 bytes), to catch mixed formats.
    lines += ["", "## Distinct 4-byte magics per extension", ""]
    for ext, files in sorted(by_ext.items()):
        mags = collections.Counter()
        for name, _ in files:
            with open(os.path.join(a.root, name), "rb") as fh:
                mags[fh.read(4)] += 1
        shown = ", ".join(f"`{m.hex()}`({''.join(chr(c) if 32 <= c < 127 else '.' for c in m)}) x{n}"
                          for m, n in mags.most_common(6))
        lines.append(f"- **{ext}**: {shown}")
    with open(a.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
    print(f"{count} files, {total:,} bytes -> {a.out}")


if __name__ == "__main__":
    main()
