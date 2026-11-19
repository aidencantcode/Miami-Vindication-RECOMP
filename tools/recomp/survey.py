"""Phase 4.1 survey: decode all code, build per-function control flow facts, report.

    python -I tools/recomp/survey.py [work/apk/lib/armeabi/libGangster2.so] [analysis/recomp_survey.md]
           [--dump-movpc N]   print the context of N `mov pc, reg` sites
"""
import collections
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from elfinfo import Elf  # noqa: E402
import thumb  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]


def decode_thumb_region(e, s, t):
    """All Thumb instructions in [s, t). BL/BLX pairs are 4 bytes."""
    out = []
    a = s
    while a + 2 <= t:
        hw = e.u16(a)
        hw2 = e.u16(a + 2) if a + 4 <= t else None
        ins = thumb.decode(a, hw, hw2)
        out.append(ins)
        a += ins.size
    return out


def main(argv):
    args, dump_movpc, i = [], 0, 0
    while i < len(argv):
        if argv[i] == "--dump-movpc":
            dump_movpc = int(argv[i + 1])
            i += 2
            continue
        args.append(argv[i])
        i += 1
    so = pathlib.Path(args[0]) if args else ROOT / "work/apk/lib/armeabi/libGangster2.so"
    report = pathlib.Path(args[1]) if len(args) > 1 else ROOT / "analysis/recomp_survey.md"
    e = Elf(str(so))
    text_lo, text_size = e.section(".text")
    plt_lo, plt_size = e.section(".plt")

    # ---- decode every Thumb region of .text ----------------------------------------------------
    insns = {}  # addr -> Insn (Thumb only)
    kind_bytes = collections.Counter()
    arm_ranges = []
    for s, t, k in e.regions(text_lo, text_lo + text_size):
        kind_bytes[k] += t - s
        if k == "t":
            for ins in decode_thumb_region(e, s, t):
                insns[ins.addr] = ins
        elif k == "a":
            arm_ranges.append((s, t))
    hist = collections.Counter((i.kind if i.kind != "alu" else "alu." + i.op) for i in insns.values())
    undef = [i for i in insns.values() if i.kind == "undef"]

    # ---- functions -------------------------------------------------------------------------------
    funcs = [f for f in e.funcs.values() if text_lo <= f.addr < text_lo + text_size]
    thumb_funcs = [f for f in funcs if f.thumb]
    arm_funcs = [f for f in funcs if not f.thumb]
    func_starts = set(f.addr for f in funcs)
    covered = bytearray(text_size)
    for f in funcs:
        lo = f.addr - text_lo
        covered[lo:lo + f.size] = b"\x01" * min(f.size, text_size - lo)
    orphan_code = sum(1 for i in insns.values() if not covered[i.addr - text_lo])

    # ---- control flow facts per instruction ------------------------------------------------------
    facts = collections.Counter()
    cross_targets = collections.Counter()   # branch (b/bcond) leaving its function -> target class
    call_targets = collections.Counter()
    mid_entries = set()
    movpc_sites = []
    for ins in insns.values():
        f = e.func_containing(ins.addr)
        inside = (lambda x: f is not None and f.addr <= x < f.addr + f.size)
        k = ins.kind
        if k in ("b", "bcond"):
            if inside(ins.target):
                facts["branch.intra"] += 1
            else:
                cls = "func_start" if ins.target in func_starts else "mid_function"
                cross_targets[f"{k}->{cls}"] += 1
                if cls == "mid_function":
                    mid_entries.add(ins.target)
        elif k == "bl":
            cls = "func_start" if ins.target in func_starts else (
                "intra" if inside(ins.target) else "mid_function")
            call_targets["bl->" + cls] += 1
            if cls == "mid_function":
                mid_entries.add(ins.target)
        elif k == "blx":
            if plt_lo <= ins.target < plt_lo + plt_size:
                call_targets["blx->plt"] += 1
            else:
                call_targets["blx->" + ("arm_func" if ins.target in func_starts else "other")] += 1
        elif k == "bx":
            facts["bx lr (return)" if ins.rm == 14 else "bx reg (indirect jump)"] += 1
        elif k == "blx_r":
            facts["blx reg (indirect call)"] += 1
        elif k == "pop" and ins.rlist & (1 << 15):
            facts["pop {..pc} (return)"] += 1
        elif k == "hmov" and ins.rd == 15:
            facts["mov pc, reg"] += 1
            movpc_sites.append(ins)
        elif k == "hadd" and ins.rd == 15:
            facts["add pc, reg"] += 1
            movpc_sites.append(ins)
        elif k in ("hadd", "hmov", "hcmp") and (ins.rd == 15 or ins.rm == 15):
            facts[f"{k} with pc operand"] += 1
        elif k == "svc":
            facts["svc"] += 1

    if dump_movpc:
        for site in movpc_sites[:dump_movpc]:
            f = e.func_containing(site.addr)
            print(f"--- {f.name if f else '?'} @ {site.addr:#x}")
            a = site.addr - 2 * 10
            while a <= site.addr + 2:
                if a in insns:
                    print("   ", insns[a])
                a += 2
            # show the words after the site (likely the table)
            print("    words after:", " ".join(f"{e.u32(((site.addr + 2 + 3) & ~3) + 4 * j):08x}" for j in range(6)),
                  "map:", e.kind_at(site.addr + 2))

    # ---- ARM code: histogram with capstone (survey only) -------------------------------------------
    arm_hist = collections.Counter()
    try:
        import capstone
        cs = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
        for s, t in arm_ranges:
            code = e.raw[e.off(s):e.off(s) + (t - s)]
            for ci in cs.disasm(code, s):
                arm_hist[ci.mnemonic] += 1
    except ImportError:
        arm_hist["(capstone missing)"] = 1

    # ---- report ----------------------------------------------------------------------------------
    L = []
    L.append("# Recompiler survey (Phase 4.1)\n")
    L.append(f"Library: `{so.name}`; `.text` {text_size:,} bytes at `{text_lo:#x}`; `.plt` {plt_size:,} bytes "
             f"({len(e.plt)} entries).\n")
    L.append("## Code map (mapping symbols in `.text`)\n")
    for k, n in sorted(kind_bytes.items()):
        L.append(f"- `${k}`: {n:,} bytes")
    L.append(f"\n## Functions\n\n- {len(funcs):,} in `.text`: {len(thumb_funcs):,} Thumb, {len(arm_funcs):,} ARM")
    L.append(f"- names with aliases: {sum(1 for v in e.aliases.values() if len(v) > 1):,} addresses")
    L.append(f"- Thumb instructions outside any function symbol: {orphan_code:,}")
    L.append(f"\n## Thumb decode\n\n- {len(insns):,} instructions, **{len(undef)} undefined**")
    if undef:
        for u in undef[:20]:
            f = e.func_containing(u.addr)
            L.append(f"  - `{u.addr:#x}` raw `{u.raw:04x}` in {f.name if f else '?'}")
    L.append("\n### Histogram\n")
    L.append("| kind | count |\n|---|---|")
    for k, n in hist.most_common():
        L.append(f"| `{k}` | {n:,} |")
    L.append("\n## Control flow\n")
    for k, n in facts.most_common():
        L.append(f"- {k}: {n:,}")
    for k, n in call_targets.most_common():
        L.append(f"- {k}: {n:,}")
    for k, n in cross_targets.most_common():
        L.append(f"- {k}: {n:,}")
    L.append(f"- distinct mid-function entry points (targets of calls/branches from elsewhere): {len(mid_entries):,}")
    L.append("\n## ARM code (capstone mnemonics)\n")
    for k, n in arm_hist.most_common():
        L.append(f"- `{k}`: {n:,}")
    # ---- program analysis (shared with the code generator) -----------------------------------
    from analyze import Program
    p = Program(e)
    jts = sum(len(f.jumptables) for f in p.funcs.values())
    jt_targets = sum(len(t) for f in p.funcs.values() for t in f.jumptables.values())
    jt_bad = [(f.name, a) for f in p.funcs.values() for a in f.jt_unresolved]
    thunks = [f for f in p.funcs.values() if f.thunk]
    problems = [(f.name, pr) for f in p.funcs.values() for pr in f.problems]
    L.append("\n## Analysis (tools/recomp/analyze.py)\n")
    L.append(f"- jump tables recovered: **{jts} of {facts['mov pc, reg']}** ({jt_targets:,} targets); unresolved: {len(jt_bad)}")
    for n, a in jt_bad[:20]:
        L.append(f"  - `{a:#x}` in {n}")
    L.append(f"- ARM thunks recognised: **{len(thunks)} of {len(arm_funcs)}**")
    L.append(f"- functions with extra entry points: {len(p.extra_entries)} ({sum(len(v) for v in p.extra_entries.values())} entries)")
    for fa, ents in list(p.extra_entries.items())[:20]:
        L.append(f"  - {p.funcs[fa].name} @ `{fa:#x}`: " + ", ".join(f"`{x:#x}`" for x in sorted(ents)))
    L.append(f"- per-function problems: {len(problems)}")
    for n, pr in problems[:30]:
        L.append(f"  - {n}: {pr}")
    # functions that can run off their end (last instruction is not a return / jump)
    def terminal(i):
        return (i.kind in ("b", "bx") or (i.kind == "pop" and i.rlist & (1 << 15))
                or (i.kind == "hmov" and i.rd == 15))
    def padding(i):  # mov r8, r8 (nop) / 0x0000 between functions
        return (i.kind == "hmov" and i.rd == 8 and i.rm == 8) or i.raw == 0
    falls = []
    for f in p.funcs.values():
        body = list(f.insns)
        while body and padding(body[-1]):
            body.pop()
        if f.thumb and body and not terminal(body[-1]):
            falls.append((f, body[-1]))
    by_kind = collections.Counter(l.kind for _, l in falls)
    L.append(f"- functions whose last instruction is not a return/jump: {len(falls)} "
             f"({', '.join(f'{k} {n}' for k, n in by_kind.most_common())})")
    for f, last in falls[:12]:
        callee = p.e.funcs.get(last.target & ~1) if last.target >= 0 else None
        L.append(f"  - {f.name[:80]}: ends with {last.kind}" + (f" -> {callee.name}" if callee else ""))
    # bx reg (not lr): context
    L.append("\n### `bx reg` (not lr) sites\n")
    bx_sites = collections.Counter()
    for f in p.funcs.values():
        for k, ins in enumerate(f.insns):
            if ins.kind == "bx" and ins.rm != 14:
                prev = f.insns[k - 1] if k else None
                key = f"bx r{ins.rm} after {prev.kind if prev else '-'}" + (
                    f" r{prev.rd}" if prev and prev.rd >= 0 else "")
                bx_sites[key] += 1
    for k, n in bx_sites.most_common():
        L.append(f"- {k}: {n}")
    report.write_text("\n".join(L) + "\n", encoding="utf-8")
    print(f"wrote {report}: {len(insns):,} Thumb insns, {len(undef)} undefined, {len(funcs):,} functions, "
          f"{facts['mov pc, reg']} mov pc, {len(mid_entries)} mid-function entries")


if __name__ == "__main__":
    main(sys.argv[1:])
