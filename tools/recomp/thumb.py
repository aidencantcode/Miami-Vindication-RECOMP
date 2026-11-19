"""Exact Thumb-1 (ARMv5T) decoder for the recompiler.

decode(addr, hw, hw2) -> Insn. hw2 is the following halfword (needed only for the 32-bit BL/BLX
pair). Every ARMv5T Thumb encoding is handled; anything else is kind="undef". Operand fields are
plain ints so the code generator can switch on `kind`.

Kinds (Thumb-1 formats, ARM ARM "THUMB instruction set"):
  lsl_i lsr_i asr_i     Rd, Rm, #imm            (format 1, flags)
  add_r sub_r           Rd, Rn, Rm              (format 2, flags)
  add_i3 sub_i3         Rd, Rn, #imm3           (format 2, flags)
  mov_i cmp_i add_i sub_i   Rd, #imm8           (format 3, flags)
  alu                   op Rd, Rm  (op = and eor lsl lsr asr adc sbc ror tst neg cmp cmn orr mul bic mvn)
  hadd hcmp hmov        Rd, Rm (any of r0-r15; hcmp sets flags)   (format 5)
  bx blx_r              Rm
  ldr_lit               Rd, [PC, #imm]  -> lit = literal address (word aligned)
  str_r strb_r ldr_r ldrb_r strh_r ldrsb_r ldrh_r ldrsh_r   Rd, [Rn, Rm]
  str_i ldr_i strb_i ldrb_i strh_i ldrh_i                   Rd, [Rn, #imm]
  str_sp ldr_sp         Rd, [SP, #imm]
  add_pc add_sp         Rd, PC|SP, #imm (add_pc: value = (addr+4)&~3 + imm)
  sp_add sp_sub         SP, #imm
  push pop              rlist (+lr / +pc)
  stmia ldmia           Rn!, rlist
  bcond                 cond, target
  svc bkpt              imm
  b                     target
  bl blx                target (blx: ARM target, word aligned)  size 4
"""
from dataclasses import dataclass, field

ALU_OPS = ["and", "eor", "lsl", "lsr", "asr", "adc", "sbc", "ror", "tst", "neg", "cmp", "cmn", "orr", "mul", "bic", "mvn"]
CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"]


@dataclass
class Insn:
    addr: int
    size: int
    kind: str
    rd: int = -1
    rn: int = -1
    rm: int = -1
    imm: int = 0
    op: str = ""
    cond: int = 14
    rlist: int = 0        # bit i = register i (push/pop include 14/15)
    target: int = -1      # branch target (absolute)
    lit: int = -1         # literal address for ldr_lit
    raw: int = 0

    def __str__(self):
        return f"{self.addr:08x}: {self.kind} {self.op} rd={self.rd} rn={self.rn} rm={self.rm} imm={self.imm:#x}" + (
            f" -> {self.target:#x}" if self.target >= 0 else "")


def _sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def decode(addr, hw, hw2=None):
    i = Insn(addr=addr, size=2, kind="undef", raw=hw)
    top3 = hw >> 13
    if top3 == 0:  # 000
        op = (hw >> 11) & 3
        if op < 3:
            i.kind = ["lsl_i", "lsr_i", "asr_i"][op]
            i.imm = (hw >> 6) & 31
            i.rm = (hw >> 3) & 7
            i.rd = hw & 7
        else:
            imm_form = (hw >> 10) & 1
            sub = (hw >> 9) & 1
            i.rn = (hw >> 3) & 7
            i.rd = hw & 7
            if imm_form:
                i.kind = "sub_i3" if sub else "add_i3"
                i.imm = (hw >> 6) & 7
            else:
                i.kind = "sub_r" if sub else "add_r"
                i.rm = (hw >> 6) & 7
        return i
    if top3 == 1:  # 001: mov/cmp/add/sub imm8
        i.kind = ["mov_i", "cmp_i", "add_i", "sub_i"][(hw >> 11) & 3]
        i.rd = (hw >> 8) & 7
        i.rn = i.rd
        i.imm = hw & 0xFF
        return i
    if top3 == 2:  # 010
        if (hw >> 10) == 0b010000:
            i.kind = "alu"
            i.op = ALU_OPS[(hw >> 6) & 15]
            i.rm = (hw >> 3) & 7
            i.rd = hw & 7
            return i
        if (hw >> 10) == 0b010001:
            op = (hw >> 8) & 3
            h1 = (hw >> 7) & 1
            h2 = (hw >> 6) & 1
            rm = ((hw >> 3) & 7) | (h2 << 3)
            rd = (hw & 7) | (h1 << 3)
            if op == 3:
                i.kind = "blx_r" if h1 else "bx"
                i.rm = rm
                return i
            i.kind = ["hadd", "hcmp", "hmov"][op]
            i.rm = rm
            i.rd = rd
            return i
        if (hw >> 11) == 0b01001:
            i.kind = "ldr_lit"
            i.rd = (hw >> 8) & 7
            i.imm = (hw & 0xFF) * 4
            i.lit = ((addr + 4) & ~3) + i.imm
            return i
        # 0101: load/store register offset
        ro = (hw >> 6) & 7
        rb = (hw >> 3) & 7
        rd = hw & 7
        i.rm, i.rn, i.rd = ro, rb, rd
        if not (hw >> 9) & 1:
            i.kind = [["str_r", "strb_r"], ["ldr_r", "ldrb_r"]][(hw >> 11) & 1][(hw >> 10) & 1]
        else:
            i.kind = ["strh_r", "ldrsb_r", "ldrh_r", "ldrsh_r"][(hw >> 10) & 3]
        return i
    if top3 == 3:  # 011: load/store imm5
        byte = (hw >> 12) & 1
        load = (hw >> 11) & 1
        imm5 = (hw >> 6) & 31
        i.rn = (hw >> 3) & 7
        i.rd = hw & 7
        i.kind = ("ldr" if load else "str") + ("b_i" if byte else "_i")
        i.imm = imm5 if byte else imm5 * 4
        return i
    if top3 == 4:  # 100
        if not (hw >> 12) & 1:  # 1000: strh/ldrh imm
            i.kind = "ldrh_i" if (hw >> 11) & 1 else "strh_i"
            i.imm = ((hw >> 6) & 31) * 2
            i.rn = (hw >> 3) & 7
            i.rd = hw & 7
        else:  # 1001: sp-relative
            i.kind = "ldr_sp" if (hw >> 11) & 1 else "str_sp"
            i.rd = (hw >> 8) & 7
            i.rn = 13
            i.imm = (hw & 0xFF) * 4
        return i
    if top3 == 5:  # 101
        if not (hw >> 12) & 1:  # 1010: add rd, pc/sp
            sp = (hw >> 11) & 1
            i.kind = "add_sp" if sp else "add_pc"
            i.rd = (hw >> 8) & 7
            i.imm = (hw & 0xFF) * 4
            if not sp:
                i.lit = ((addr + 4) & ~3) + i.imm
            return i
        # 1011: misc
        if (hw >> 8) == 0b10110000:
            i.kind = "sp_sub" if (hw >> 7) & 1 else "sp_add"
            i.imm = (hw & 0x7F) * 4
            return i
        if ((hw >> 9) & 3) == 0b10:  # 1011 x10R: push/pop
            load = (hw >> 11) & 1
            r = (hw >> 8) & 1
            i.kind = "pop" if load else "push"
            i.rlist = (hw & 0xFF) | ((1 << (15 if load else 14)) if r else 0)
            return i
        if (hw >> 8) == 0b10111110:
            i.kind = "bkpt"
            i.imm = hw & 0xFF
            return i
        return i  # undef in ARMv5T (v6 extension space)
    if top3 == 6:  # 110
        if not (hw >> 12) & 1:  # 1100 stmia/ldmia
            i.kind = "ldmia" if (hw >> 11) & 1 else "stmia"
            i.rn = (hw >> 8) & 7
            i.rlist = hw & 0xFF
            return i
        cond = (hw >> 8) & 15
        if cond == 15:
            i.kind = "svc"
            i.imm = hw & 0xFF
            return i
        if cond == 14:
            return i  # undefined
        i.kind = "bcond"
        i.cond = cond
        i.target = addr + 4 + _sext(hw & 0xFF, 8) * 2
        return i
    # top3 == 7: 111
    h = (hw >> 11) & 3
    if h == 0:
        i.kind = "b"
        i.target = addr + 4 + _sext(hw & 0x7FF, 11) * 2
        return i
    if h == 2 and hw2 is not None:  # BL/BLX prefix + suffix
        h2 = (hw2 >> 11) & 0x1F
        off = (_sext(hw & 0x7FF, 11) << 12) | ((hw2 & 0x7FF) << 1)
        if h2 == 0b11111:
            i.kind = "bl"
            i.size = 4
            i.target = (addr + 4 + off) & 0xFFFFFFFF
            return i
        if h2 == 0b11101 and not (hw2 & 1):
            i.kind = "blx"
            i.size = 4
            i.target = ((addr + 4 + off) & ~3) & 0xFFFFFFFF
            return i
    return i  # lone prefix/suffix: undefined here


def regs(rlist):
    return [r for r in range(16) if rlist & (1 << r)]
