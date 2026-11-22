"""Phase 4.2: translate libGangster2.so into C (see src/recomp/rc.h for the runtime contract).

    python -I tools/recomp/codegen.py [--out work/recomp] [--per-file 200] [--only NAME[,NAME...]]
                                      [--probe NAME[,NAME...]]   (extra hook checks, for debugging)

Output (git-ignored, derived from the game's code):
    rc_funcs.h      prototypes of every translated function
    rc_part_NNN.c   function bodies
    rc_table.c      sorted address -> function table, names, import names
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from analyze import Program  # noqa: E402
from elfinfo import Elf  # noqa: E402
import thumb  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
LIB_BASE = 0x10000000
CONDS = thumb.CONDS

# Imported runtime helpers emitted inline (same semantics as src/cpu/fastpath.S, which is
# checked bit-exact against host IEEE math by --selftest-fastpath).
INLINE_IMPORTS = {
    "__aeabi_fadd": "R(0) = rc_fb(rc_f(R(0)) + rc_f(R(1)));",
    "__aeabi_fsub": "R(0) = rc_fb(rc_f(R(0)) - rc_f(R(1)));",
    "__aeabi_fmul": "R(0) = rc_fb(rc_f(R(0)) * rc_f(R(1)));",
    "__aeabi_fdiv": "R(0) = rc_fb(rc_f(R(0)) / rc_f(R(1)));",
    "__aeabi_fcmpeq": "R(0) = rc_f(R(0)) == rc_f(R(1));",
    "__aeabi_fcmplt": "R(0) = rc_f(R(0)) < rc_f(R(1));",
    "__aeabi_fcmple": "R(0) = rc_f(R(0)) <= rc_f(R(1));",
    "__aeabi_fcmpge": "R(0) = rc_f(R(0)) >= rc_f(R(1));",
    "__aeabi_fcmpgt": "R(0) = rc_f(R(0)) > rc_f(R(1));",
    "__aeabi_f2iz": "R(0) = rc_f2iz(rc_f(R(0)));",
    "__aeabi_f2d": "rc_retd(s, (double)rc_f(R(0)));",
    "__aeabi_i2f": "R(0) = rc_fb((float)(int32_t)R(0));",
    "__aeabi_ui2f": "R(0) = rc_fb((float)R(0));",
    "__aeabi_dadd": "rc_retd(s, rc_d(R(0), R(1)) + rc_d(R(2), R(3)));",
    "__aeabi_dsub": "rc_retd(s, rc_d(R(0), R(1)) - rc_d(R(2), R(3)));",
    "__aeabi_dmul": "rc_retd(s, rc_d(R(0), R(1)) * rc_d(R(2), R(3)));",
    "__aeabi_ddiv": "rc_retd(s, rc_d(R(0), R(1)) / rc_d(R(2), R(3)));",
    "__aeabi_dcmpeq": "R(0) = rc_d(R(0), R(1)) == rc_d(R(2), R(3));",
    "__aeabi_dcmplt": "R(0) = rc_d(R(0), R(1)) < rc_d(R(2), R(3));",
    "__aeabi_dcmple": "R(0) = rc_d(R(0), R(1)) <= rc_d(R(2), R(3));",
    "__aeabi_dcmpge": "R(0) = rc_d(R(0), R(1)) >= rc_d(R(2), R(3));",
    "__aeabi_dcmpgt": "R(0) = rc_d(R(0), R(1)) > rc_d(R(2), R(3));",
    "__aeabi_dcmpun": "{ double a_ = rc_d(R(0), R(1)), b_ = rc_d(R(2), R(3)); R(0) = (a_ != a_) || (b_ != b_); }",
    "__aeabi_d2f": "R(0) = rc_fb((float)rc_d(R(0), R(1)));",
    "__aeabi_d2iz": "R(0) = rc_d2iz(rc_d(R(0), R(1)));",
    "__aeabi_d2uiz": "R(0) = rc_d2uiz(rc_d(R(0), R(1)));",
    "__aeabi_i2d": "rc_retd(s, (double)(int32_t)R(0));",
    "__aeabi_ui2d": "rc_retd(s, (double)R(0));",
    "__aeabi_idiv": "R(0) = rc_sdiv(R(0), R(1));",
    "__aeabi_uidiv": "R(0) = rc_udiv(R(0), R(1));",
    "__aeabi_idivmod": "{ uint32_t q_ = rc_sdiv(R(0), R(1)); R(1) = R(1) ? R(0) - q_ * R(1) : R(0); R(0) = q_; }",
    "__aeabi_uidivmod": "{ uint32_t q_ = rc_udiv(R(0), R(1)); R(1) = R(1) ? R(0) - q_ * R(1) : R(0); R(0) = q_; }",
    "__aeabi_lmul": "{ uint64_t a_ = ((uint64_t)R(1) << 32) | R(0), b_ = ((uint64_t)R(3) << 32) | R(2), "
                    "p_ = a_ * b_; R(0) = (uint32_t)p_; R(1) = (uint32_t)(p_ >> 32); }",
    "sqrtf": "R(0) = rc_fb(sqrtf(rc_f(R(0))));",
    "sqrt": "rc_retd(s, sqrt(rc_d(R(0), R(1))));",
}
# Game-defined functions replaced by C (they are not Thumb or need host semantics).
NATIVE_FUNCS = {
    "__aeabi_f2uiz": "R(0) = rc_f2uiz(rc_f(R(0)));",
}

# Functions MSVC's optimizer needs 6+ minutes for (measured with /d2cgsummary): the gameswf debug
# disassemblers, only used for logging. Compiled without optimization on MSVC.
NO_OPTIMIZE = {
    "_ZN7gameswf15log_disasm_avm2ERKNS_6membufEPKNS_7abc_defE",
    "_ZN7gameswf10log_disasmEPKh",
}

# Functions that get hook checks at entry/exit (rc_hook_enter / rc_hook_leave in rc_runtime.cpp),
# so rt::Runtime::hook_function works in recomp mode. Keep in sync with src/game/mods.cpp; extra
# names for debugging come from --probe NAME[,NAME...].
HOOKABLE = {
    "_ZN9GS3DStuff22loadPerformanceProfileEPKc",
    "_ZN11CHudManager4loadEv",
    "_ZNK11AnalogStick6draw2dEv", "_ZNK14AnimatedButton6draw2dEv", "_ZNK10HudElement6draw2dEv",
    "_ZNK12SlideControl6draw2dEv", "_ZNK11SwipeButton6draw2dEv", "_ZNK12ToggleButton6draw2dEv",
    "_ZNK5Wheel6draw2dEv",
    "_ZN11Application7_UpdateEi",
    "_ZN12FollowCamera6updateEf",
    "_ZN13CameraManager6updateEi",
    "_ZN12FollowCamera12updateAnglesEii",
    "_ZN5Menus8MainMenu8GotFocusEv",
    "_ZN5Menus8MainMenu6NotifyEPK10MenuWidgeti",
    "_ZN21DrivingControlHandler18handleAccelerationEPK14EvAcceleration",
    "_ZN7DayTime6updateEd",  # time-of-day override (GANGSTAR_TIME)
    "_ZN9HudScreen19processTouchReleaseEv",  # aiming: no tap-to-target (5A.4)
    "_ZN9Character19getAttackableTargetEv",  # aiming: no automatic lock (5A.4)
    "_ZN6Weapon5shootEP9CharacterN6glitch4core8vector3dIfEES5_b",  # manual aim: shots to the crosshair
}


def A(a):
    return f"0x{(LIB_BASE + a) & 0xFFFFFFFF:08x}u"


def fname(addr):
    return f"f_{addr:06x}"


def cmt(name):
    return name.replace("*/", "* /")


class Gen:
    def __init__(self, p):
        self.p = p
        self.e = p.e
        self.imports = sorted(set(self.e.plt.values()))
        self.import_idx = {n: i for i, n in enumerate(self.imports)}
        self.stats = {"functions": 0, "insns": 0, "unhandled": 0}
        self.needs_setjmp = False
        self.hookable = set(HOOKABLE)

    def val(self, ins, r):
        """Value of a register operand (pc reads as a constant)."""
        return A(ins.addr + 4) if r == 15 else f"R({r})"

    def insn(self, f, ins):
        k, d, n, m, imm = ins.kind, ins.rd, ins.rn, ins.rm, ins.imm
        ret = f"{A(ins.addr + ins.size)} | 1u"
        if k == "lsl_i":
            if imm == 0:
                return [f"R({d}) = R({m}); NZ(R({d}));"]
            return [f"c_ = (R({m}) >> {32 - imm}) & 1; R({d}) = R({m}) << {imm}; NZ(R({d}));"]
        if k == "lsr_i":
            if imm == 0:
                return [f"c_ = R({m}) >> 31; R({d}) = 0; NZ(0);"]
            return [f"c_ = (R({m}) >> {imm - 1}) & 1; R({d}) = R({m}) >> {imm}; NZ(R({d}));"]
        if k == "asr_i":
            if imm == 0:
                return [f"c_ = R({m}) >> 31; R({d}) = (uint32_t)((int32_t)R({m}) >> 31); NZ(R({d}));"]
            return [f"c_ = ((uint32_t)((int32_t)R({m}) >> {imm - 1})) & 1; "
                    f"R({d}) = (uint32_t)((int32_t)R({m}) >> {imm}); NZ(R({d}));"]
        if k == "add_r":
            return [f"ADDS(R({d}), R({n}), R({m}));"]
        if k == "sub_r":
            return [f"SUBS(R({d}), R({n}), R({m}));"]
        if k == "add_i3":
            return [f"ADDS(R({d}), R({n}), {imm}u);"]
        if k == "sub_i3":
            return [f"SUBS(R({d}), R({n}), {imm}u);"]
        if k == "mov_i":
            return [f"R({d}) = {imm}u; NZ(R({d}));"]
        if k == "cmp_i":
            return [f"CMP(R({d}), {imm}u);"]
        if k == "add_i":
            return [f"ADDS(R({d}), R({d}), {imm}u);"]
        if k == "sub_i":
            return [f"SUBS(R({d}), R({d}), {imm}u);"]
        if k == "alu":
            op = ins.op
            simple = {"and": "&", "eor": "^", "orr": "|"}
            if op in simple:
                return [f"R({d}) {simple[op]}= R({m}); NZ(R({d}));"]
            if op == "bic":
                return [f"R({d}) &= ~R({m}); NZ(R({d}));"]
            if op == "mvn":
                return [f"R({d}) = ~R({m}); NZ(R({d}));"]
            if op in ("lsl", "lsr", "asr", "ror"):
                return [f"R({d}) = rc_{op}(R({d}), R({m}), &c_); NZ(R({d}));"]
            if op == "adc":
                return [f"ADCS(R({d}), R({d}), R({m}));"]
            if op == "sbc":
                return [f"SBCS(R({d}), R({d}), R({m}));"]
            if op == "tst":
                return [f"NZ(R({d}) & R({m}));"]
            if op == "neg":
                return [f"SUBS(R({d}), 0u, R({m}));"]
            if op == "cmp":
                return [f"CMP(R({d}), R({m}));"]
            if op == "cmn":
                return [f"CMN(R({d}), R({m}));"]
            if op == "mul":
                return [f"R({d}) = R({d}) * R({m}); NZ(R({d}));"]
        if k == "hadd":
            if d == 15:
                return self.unhandled(ins, "add pc, reg")
            return [f"R({d}) = R({d}) + {self.val(ins, m)};"]
        if k == "hcmp":
            return [f"CMP({self.val(ins, d)}, {self.val(ins, m)});"]
        if k == "hmov":
            if d == 15:
                return self.jump_table(f, ins)
            if d == m:
                return []  # mov r8, r8 (nop)
            return [f"R({d}) = {self.val(ins, m)};"]
        if k == "bx":
            return ["return;"]  # bx lr, or bx r3 right after popping the return address into r3
        if k == "blx_r":
            return [f"{{ uint32_t t_ = R({m}); LR = {ret}; rc_call(s, t_); }}"]
        if k == "ldr_lit":
            return [f"R({d}) = 0x{self.e.u32(ins.lit):08x}u;"]
        mem = {"str_r": ("WR32", "R({n}) + R({m})"), "strb_r": ("WR8", "R({n}) + R({m})"),
               "strh_r": ("WR16", "R({n}) + R({m})"),
               "str_i": ("WR32", "R({n}) + {imm}u"), "strb_i": ("WR8", "R({n}) + {imm}u"),
               "strh_i": ("WR16", "R({n}) + {imm}u"), "str_sp": ("WR32", "SP + {imm}u")}
        if k in mem:
            fn, addr = mem[k]
            return [f"{fn}({addr.format(n=n, m=m, imm=imm)}, R({d}));"]
        loads = {"ldr_r": ("RD32", "R({n}) + R({m})"), "ldrb_r": ("RD8", "R({n}) + R({m})"),
                 "ldrh_r": ("RD16", "R({n}) + R({m})"), "ldrsb_r": ("RDS8", "R({n}) + R({m})"),
                 "ldrsh_r": ("RDS16", "R({n}) + R({m})"),
                 "ldr_i": ("RD32", "R({n}) + {imm}u"), "ldrb_i": ("RD8", "R({n}) + {imm}u"),
                 "ldrh_i": ("RD16", "R({n}) + {imm}u"), "ldr_sp": ("RD32", "SP + {imm}u")}
        if k in loads:
            fn, addr = loads[k]
            return [f"R({d}) = {fn}({addr.format(n=n, m=m, imm=imm)});"]
        if k == "add_pc":
            return [f"R({d}) = {A(((ins.addr + 4) & ~3) + imm)};"]
        if k == "add_sp":
            return [f"R({d}) = SP + {imm}u;"]
        if k == "sp_add":
            return [f"SP += {imm}u;"]
        if k == "sp_sub":
            return [f"SP -= {imm}u;"]
        if k == "push":
            rs = thumb.regs(ins.rlist)
            body = " ".join(f"WR32(a_ + {4 * i}u, R({r}));" for i, r in enumerate(rs))
            return [f"{{ uint32_t a_ = SP - {4 * len(rs)}u; {body} SP = a_; }}"]
        if k == "pop":
            rs = thumb.regs(ins.rlist)
            body = " ".join(f"R({r}) = RD32(a_ + {4 * i}u);" for i, r in enumerate(rs) if r != 15)
            line = f"{{ uint32_t a_ = SP; {body} SP = a_ + {4 * len(rs)}u; }}"
            return [line, "return;"] if 15 in rs else [line]
        if k == "stmia":
            rs = thumb.regs(ins.rlist)
            body = " ".join(f"WR32(a_ + {4 * i}u, R({r}));" for i, r in enumerate(rs))
            return [f"{{ uint32_t a_ = R({n}); {body} R({n}) = a_ + {4 * len(rs)}u; }}"]
        if k == "ldmia":
            rs = thumb.regs(ins.rlist)
            body = " ".join(f"R({r}) = RD32(a_ + {4 * i}u);" for i, r in enumerate(rs))
            wb = "" if n in rs else f" R({n}) = a_ + {4 * len(rs)}u;"
            return [f"{{ uint32_t a_ = R({n}); {body}{wb} }}"]
        if k == "bcond":
            return [f"if (C_{CONDS[ins.cond]}) goto L_{ins.target:06x};"]
        if k == "b":
            if f.addr <= ins.target < f.end:
                return [f"goto L_{ins.target:06x};"]
            return [f"{fname(ins.target)}(s); return; /* tail branch */"]
        if k == "bl":
            # bl to a label inside this function is GCC's Thumb-1 "far jump" (LR is saved in the
            # prologue); bl to the function's own entry is plain recursion (rb-tree _M_erase etc.).
            if f.addr < ins.target < f.end:
                return [f"LR = {ret}; goto L_{ins.target:06x}; /* far jump */"]
            if ins.target in self.p.funcs:
                return [f"LR = {ret}; {fname(ins.target)}(s);"]
            return [f"LR = {ret}; rc_call(s, {A(ins.target)} | 1u);"]
        if k == "blx":
            t = ins.target
            if t in self.e.plt:
                name = self.e.plt[t]
                if name in INLINE_IMPORTS:
                    return [INLINE_IMPORTS[name] + f" /* {name} */"]
                if name == "setjmp":
                    # Returns twice: the host setjmp sits in this function's frame, and the runtime's
                    # longjmp (rc_runtime.cpp) restores r4-r11/sp, sets r0 and host-longjmps back here.
                    self.needs_setjmp = True
                    return [f"LR = {ret}; if (setjmp(*(jmp_buf*)rc_setjmp_slot(s))) {{}} /* setjmp */"]
                return [f"LR = {ret}; rc_import(s, {self.import_idx[name]}); /* {cmt(name)} */"]
            if t in self.p.funcs:
                return [f"LR = {ret}; {fname(t)}(s);"]
            return [f"LR = {ret}; rc_call(s, {A(t)});"]
        return self.unhandled(ins, k)

    def unhandled(self, ins, what):
        self.stats["unhandled"] += 1
        return [f'rc_unhandled(s, {A(ins.addr)}, "{what}"); return;']

    def jump_table(self, f, ins):
        targets = f.jumptables.get(ins.addr)
        if not targets:
            return self.unhandled(ins, "mov pc, reg without a recovered table")
        out = [f"switch (R({ins.rm}) & ~1u) {{"]
        for t in sorted(set(targets)):
            out.append(f"  case {A(t)}: goto L_{t:06x};")
        out.append(f"  default: rc_bad_jump(s, {A(ins.addr)}, R({ins.rm})); return;")
        out.append("}")
        return out

    def function(self, f):
        self.stats["functions"] += 1
        L = [f"/* {cmt(f.name)} */", f"void {fname(f.addr)}(RcState* s) {{"]
        if f.name in NATIVE_FUNCS:
            L += [f"  {NATIVE_FUNCS[f.name]}", "}", ""]
            return L
        if not f.thumb:
            if not f.thunk:
                L += [f'  rc_unhandled(s, {A(f.addr)}, "ARM function");', "}", ""]
                return L
            adj, target = f.thunk
            for r, dv in sorted(adj.items()):
                L.append(f"  R({r}) += {dv & 0xFFFFFFFF:#x}u;")
            t = target & ~1
            L.append(f"  {fname(t)}(s);" if t in self.p.funcs else f"  rc_call(s, {A(target)});")
            L += ["}", ""]
            return L
        L.append("  RC_FLAGS;")
        for ins in f.insns:
            self.stats["insns"] += 1
            if ins.addr in f.labels:
                L.append(f"L_{ins.addr:06x}:;")
            for line in self.insn(f, ins):
                L.append("  " + line)
        L.append(f"  rc_fell_off(s, {A(f.addr)});")
        L += ["}", ""]
        if self.needs_setjmp:
            self.needs_setjmp = False
            L = ["#include <setjmp.h>", "void* rc_setjmp_slot(RcState* s); /* rc_runtime.cpp */"] + L
        names = {f.name, *self.e.aliases.get(f.addr, [])}
        if names & self.hookable:
            # Entry/exit hook checks around the original body (renamed *_body, file-local).
            body = fname(f.addr) + "_body"
            L[1] = f"static void {body}(RcState* s) {{"
            L = ["int rc_hook_enter(RcState* s, uint32_t addr); void rc_hook_leave(RcState* s, uint32_t addr);",
                 f"static void {body}(RcState* s);",
                 f"/* {cmt(f.name)} (hookable) */",
                 f"void {fname(f.addr)}(RcState* s) {{",
                 f"  if (rc_hook_enter(s, {A(f.addr)})) return;",
                 f"  {body}(s);",
                 f"  rc_hook_leave(s, {A(f.addr)});",
                 "}"] + L
        if f.name in NO_OPTIMIZE:
            msvc = "#if defined(_MSC_VER) && !defined(__clang__)"
            L = [msvc, '#pragma optimize("", off)', "#endif"] + L + [msvc, '#pragma optimize("", on)', "#endif", ""]
        return L


def write_if_changed(path, text):
    """Keep the timestamp of unchanged files so the build only recompiles what changed."""
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return False
    path.write_text(text, encoding="utf-8")
    return True


def main(argv):
    out = ROOT / "work/recomp"
    per_file = 200
    only = None
    probes = set()
    i = 0
    while i < len(argv):
        if argv[i] == "--out":
            out = pathlib.Path(argv[i + 1]); i += 2; continue
        if argv[i] == "--per-file":
            per_file = int(argv[i + 1]); i += 2; continue
        if argv[i] == "--probe":
            probes = set(argv[i + 1].split(",")); i += 2; continue
        if argv[i] == "--only":
            only = set(argv[i + 1].split(",")); i += 2; continue
        i += 1
    e = Elf(str(ROOT / "work/apk/lib/armeabi/libGangster2.so"))
    p = Program(e)
    g = Gen(p)
    g.hookable |= probes
    out.mkdir(parents=True, exist_ok=True)
    funcs = sorted(p.funcs.values(), key=lambda f: f.addr)
    if only:
        funcs = [f for f in funcs if f.name in only]
    hdr = ["/* generated by tools/recomp/codegen.py: do not edit */", "#pragma once", '#include "rc.h"', ""]
    hdr += [f"void {fname(f.addr)}(RcState* s);" for f in sorted(p.funcs.values(), key=lambda f: f.addr)]
    changed = int(write_if_changed(out / "rc_funcs.h", "\n".join(hdr) + "\n"))
    prelude = ["/* generated by tools/recomp/codegen.py: do not edit */",
               "#include <math.h>", '#include "rc_funcs.h"',
               "#if defined(_MSC_VER) && !defined(__clang__)",
               "#pragma warning(disable: 4102 4146 4189 4245 4389 4701 4702)",
               "#elif defined(__clang__)",
               '#pragma clang diagnostic ignored "-Wunused-label"',
               '#pragma clang diagnostic ignored "-Wunused-variable"',
               "#endif", ""]
    nfiles = 0
    for start in range(0, len(funcs), per_file):
        body = list(prelude)
        for f in funcs[start:start + per_file]:
            body += g.function(f)
        changed += write_if_changed(out / f"rc_part_{nfiles:03d}.c", "\n".join(body) + "\n")
        nfiles += 1
    for old in out.glob("rc_part_*.c"):  # parts left over from a run with more files
        if int(old.stem.split("_")[-1]) >= nfiles:
            old.unlink()
    # dispatch table + names
    allf = sorted(p.funcs.values(), key=lambda f: f.addr)
    tab = ["/* generated by tools/recomp/codegen.py: do not edit */", '#include "rc_funcs.h"', ""]
    tab.append(f"const uint32_t rc_fn_count = {len(allf)};")
    tab.append("const uint32_t rc_fn_addr[] = {")
    tab += [f"  {A(f.addr)}," for f in allf]
    tab.append("};")
    tab.append("const RcFn rc_fn_ptr[] = {")
    tab += [f"  {fname(f.addr)}," for f in allf]
    tab.append("};")
    tab.append("const char* const rc_fn_name[] = {")
    tab += [f'  "{f.name}",' for f in allf]
    tab.append("};")
    tab.append(f"const uint32_t rc_import_count = {len(g.imports)};")
    tab.append("const char* const rc_import_name[] = {")
    tab += [f'  "{n}",' for n in g.imports]
    tab.append("};")
    changed += write_if_changed(out / "rc_table.c", "\n".join(tab) + "\n")
    print(f"generated {g.stats['functions']} functions ({g.stats['insns']:,} instructions, "
          f"{g.stats['unhandled']} unhandled) into {nfiles} files + rc_table.c in {out} ({changed} files changed)")


if __name__ == "__main__":
    main(sys.argv[1:])
