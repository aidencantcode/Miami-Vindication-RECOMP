"""Program analysis shared by the survey and the code generator.

Program(elf) builds, for every function in .text:
  - Thumb functions: the decoded instructions (data islands skipped), labels (intra-function
    branch targets), recovered jump tables ("mov pc, reg" sites -> list of targets)
  - ARM functions: the C++ non-virtual thunk pattern (adjust r0, tail-call a target)
plus the set of mid-function addresses that other code branches/calls into ("extra entries").
"""
import collections
from dataclasses import dataclass, field

import thumb

THUMB_BRANCHES = ("b", "bcond")


@dataclass
class Function:
    name: str
    addr: int
    size: int
    thumb: bool
    insns: list = field(default_factory=list)
    labels: set = field(default_factory=set)
    jumptables: dict = field(default_factory=dict)   # site addr -> [targets (Thumb bit cleared)]
    jt_unresolved: list = field(default_factory=list)
    thunk: tuple = None                               # ARM: (r0 adjustment, target with Thumb bit)
    problems: list = field(default_factory=list)

    @property
    def end(self):
        return self.addr + self.size


class Program:
    def __init__(self, e):
        self.e = e
        self.text_lo, text_size = e.section(".text")
        self.text_hi = self.text_lo + text_size
        self.plt_lo, plt_size = e.section(".plt")
        self.plt_hi = self.plt_lo + plt_size
        self.funcs = {}
        for f in e.funcs.values():
            if self.text_lo <= f.addr < self.text_hi:
                self.funcs[f.addr] = Function(f.name, f.addr, f.size, f.thumb)
        self.starts = set(self.funcs)
        for f in self.funcs.values():
            if f.thumb:
                self._decode_thumb(f)
            else:
                self._arm_thunk(f)
        self.extra_entries = collections.defaultdict(set)  # function addr -> mid-function entries
        for f in self.funcs.values():
            if f.thumb:
                self._labels(f)
                self._jumptables(f)

    # ---- decoding --------------------------------------------------------------------------------
    def _decode_thumb(self, f):
        e = self.e
        for s, t, k in e.regions(f.addr, f.end):
            if k == "d":
                continue  # literal pool / jump table data
            a = s
            while a + 2 <= t:
                hw = e.u16(a)
                hw2 = e.u16(a + 2) if a + 4 <= t else None
                ins = thumb.decode(a, hw, hw2)
                f.insns.append(ins)
                a += ins.size
        bad = [i for i in f.insns if i.kind == "undef"]
        if bad:
            f.problems.append(f"{len(bad)} undefined instructions (first {bad[0].addr:#x})")

    def _arm_thunk(self, f):
        """ldr ip,[pc,#k]; add ip,pc,ip; (sub|add r0,r0,#imm)+; bx ip; .word off"""
        e = self.e
        w = [e.u32(f.addr + 4 * i) for i in range(f.size // 4)]
        if len(w) < 4 or (w[0] & 0xFFFFF000) != 0xE59FC000 or w[1] != 0xE08FC00C:
            f.problems.append("ARM function is not a thunk")
            return
        lit_addr = f.addr + 8 + (w[0] & 0xFFF)
        target = (f.addr + 4 + 8 + e.u32(lit_addr)) & 0xFFFFFFFF
        adj = collections.Counter()  # register -> delta

        def imm(x):
            v, r = x & 0xFF, ((x >> 8) & 0xF) * 2
            return ((v >> r) | (v << (32 - r))) & 0xFFFFFFFF if r else v

        i = 2
        while i < len(w) and w[i] != 0xE12FFF1C:
            op, rn, rd = w[i] & 0xFFF00000, (w[i] >> 16) & 15, (w[i] >> 12) & 15
            if op in (0xE2400000, 0xE2800000) and rn == rd and rd < 4:   # sub/add rX, rX, #imm
                adj[rd] += -imm(w[i]) if op == 0xE2400000 else imm(w[i])
            else:
                f.problems.append(f"unexpected ARM word {w[i]:08x} in thunk")
                return
            i += 1
        if i >= len(w):
            f.problems.append("thunk without bx ip")
            return
        f.thunk = (dict(adj), target)

    # ---- control flow ------------------------------------------------------------------------
    def _labels(self, f):
        f.labels.add(f.addr)
        for ins in f.insns:
            t = ins.target
            if ins.kind in THUMB_BRANCHES or (ins.kind == "bl" and f.addr <= t < f.end):
                if f.addr <= t < f.end:
                    f.labels.add(t)
                else:
                    g = self.func_at(t)
                    if g and t != g.addr:
                        self.extra_entries[g.addr].add(t)
            elif ins.kind == "bl" and t not in self.starts:
                g = self.func_at(t)
                if g and t != g.addr:
                    self.extra_entries[g.addr].add(t)

    def func_at(self, a):
        f = self.e.func_containing(a)
        return self.funcs.get(f.addr) if f else None

    def _jumptables(self, f):
        """GCC Thumb-1 PIC switch, in several register shapes, always:
             ldr rL, =LIT ... table = GOT + LIT ... ldr rE, [table, idx*4] ; add rD, rE, table ; mov pc, rD
        Entries are offsets from the table. ARMv5T `mov pc` does not interwork, so targets may have
        bit 0 clear. The bound comes from the nearest preceding `cmp rX, #N` + conditional branch:
        hi/ls -> N+1 entries, cs/cc -> N. Each literal loaded in the window is tried; a candidate
        is accepted when every target lands inside the function (and on an instruction)."""
        by_addr = {i.addr: n for n, i in enumerate(f.insns)}

        def guard(w):
            """Entry count from `cmp rX, #N` right before a hi/ls/cs/cc branch."""
            if w.kind != "bcond" or w.cond not in (8, 9, 2, 3):
                return None
            j = by_addr.get(w.addr - 2)
            prev = f.insns[j] if j is not None else None
            if not (prev and prev.kind == "cmp_i"):
                return None
            return prev.imm + 1 if w.cond in (8, 9) else prev.imm

        def no_fallthrough(w):
            return (w.kind in ("b", "bx") or (w.kind == "hmov" and w.rd == 15)
                    or (w.kind == "pop" and 15 in thumb.regs(w.rlist))
                    or (w.kind == "bl" and f.addr < w.target < f.end))  # far jump

        for n, ins in enumerate(f.insns):
            if not (ins.kind == "hmov" and ins.rd == 15):
                continue
            window = f.insns[max(0, n - 24):n]
            count = None
            # The bound check on the path into the jump: walk back through the code that falls
            # through into it (never across a b/bx/pop pc: code above those belongs to another
            # path, e.g. a neighbouring switch). If the walk hits such a boundary first, the
            # table block is only entered by branches, so use the guarded branch that targets it
            # (`cmp r3, #12; bls table_block` elsewhere in the function).
            block_start = ins.addr
            for w in reversed(window):
                if no_fallthrough(w):
                    break
                block_start = w.addr
                count = guard(w)
                if count is not None:
                    break
            if count is None:
                into = [guard(w) for w in f.insns if w.kind == "bcond" and block_start <= w.target <= ins.addr]
                into = [c for c in into if c is not None]
                if into:
                    count = max(into)
            found = None
            # nearest literals first; then (table address computed once and kept in a register or
            # on the stack) every literal of the function, but only with a known entry count
            lits = [w for w in reversed(window) if w.kind == "ldr_lit"]
            if count is not None:
                lits += [w for w in f.insns if w.kind == "ldr_lit" and w not in lits]
            for lit in lits:
                try:
                    table = (self.e.got_base + self.e.u32(lit.lit)) & 0xFFFFFFFF
                except ValueError:
                    continue
                targets = []
                for k in range(count if count is not None else 1024):
                    try:
                        t = ((table + self.e.u32(table + 4 * k)) & 0xFFFFFFFF) & ~1
                    except ValueError:
                        break
                    if not (f.addr <= t < f.end and t in by_addr):
                        break
                    targets.append(t)
                if targets and (count is None or len(targets) == count):
                    found = targets
                    break
            if not found:  # last resort: any literal, unbounded (generated code fails loudly on a miss)
                for lit in [w for w in f.insns if w.kind == "ldr_lit"]:
                    try:
                        table = (self.e.got_base + self.e.u32(lit.lit)) & 0xFFFFFFFF
                    except ValueError:
                        continue
                    targets = []
                    for k in range(1024):
                        try:
                            t = ((table + self.e.u32(table + 4 * k)) & 0xFFFFFFFF) & ~1
                        except ValueError:
                            break
                        if not (f.addr <= t < f.end and t in by_addr):
                            break
                        targets.append(t)
                    if len(targets) >= 2:
                        found = targets
                        f.problems.append(f"jump table at {ins.addr:#x}: unbounded guess, {len(targets)} entries")
                        break
            if not found:
                f.jt_unresolved.append(ins.addr)
                continue
            f.jumptables[ins.addr] = found
            f.labels.update(found)
