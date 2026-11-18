"""Checks the Unicorn behaviours the probe relies on. Run before trusting the probe
on a new Unicorn version:  python -I tools/probe/test_unicorn_semantics.py

1. Inside an SVC interrupt hook, what does PC read as?
2. Can a hook redirect execution by writing PC (with the Thumb bit)?
3. Can a hook run guest code with a nested emu_start and then resume?
"""
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_INTR
from unicorn.arm_const import (UC_ARM_REG_PC, UC_ARM_REG_LR, UC_ARM_REG_SP,
                               UC_ARM_REG_R0, UC_ARM_REG_CPSR, UC_CPU_ARM_926)

SVC0, SVC1, SVC2, SVC3 = 0xEF000000, 0xEF000001, 0xEF000002, 0xEF000003
BX_LR = 0xE12FFF1E
CODE = 0x10000
SENT = 0x20000        # svc #1 -> stop
SENT2 = 0x20010       # svc #2 -> stop (nested)
THUMB_FN = 0x30000    # movs r0,#42 ; bx lr


def setup():
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.ctl_set_cpu_model(UC_CPU_ARM_926)
    uc.mem_map(0x10000, 0x30000)
    uc.mem_map(0x70000, 0x10000)  # stack
    uc.mem_write(SENT, struct.pack("<I", SVC1))
    uc.mem_write(SENT2, struct.pack("<I", SVC2))
    uc.mem_write(THUMB_FN, struct.pack("<HH", 0x202A, 0x4770))
    return uc


def run(uc, start):
    uc.reg_write(UC_ARM_REG_SP, 0x7FF00)
    uc.reg_write(UC_ARM_REG_LR, SENT)
    uc.emu_start(start, 0xFFFFFFF0, count=10000)
    return uc.reg_read(UC_ARM_REG_R0)


def main():
    ok = True
    # 1 + 2: PC in hook, and redirect via PC write
    uc = setup()
    uc.mem_write(CODE, struct.pack("<II", SVC0, BX_LR))
    seen = {}

    def hook(uc, intno, _):
        pc = uc.reg_read(UC_ARM_REG_PC)
        word = struct.unpack("<I", uc.mem_read(pc - 4, 4))[0]
        imm = word & 0xFFFFFF
        seen.setdefault("pc", pc); seen.setdefault("intno", intno)
        if imm == 0:
            uc.reg_write(UC_ARM_REG_PC, THUMB_FN | 1)   # redirect, keep LR
        elif imm in (1, 2):
            uc.emu_stop()
    uc.hook_add(UC_HOOK_INTR, hook)
    r0 = run(uc, CODE)
    print(f"1. PC inside SVC hook = {seen['pc']:#x} (svc at {CODE:#x}), intno={seen['intno']}")
    print(f"2. redirect via PC write -> r0={r0} (expect 42)")
    ok &= seen["pc"] == CODE + 4 and r0 == 42

    # 3: nested emu_start from inside a hook
    uc = setup()
    uc.mem_write(CODE, struct.pack("<II", SVC3, BX_LR))

    def hook2(uc, intno, _):
        pc = uc.reg_read(UC_ARM_REG_PC)
        imm = struct.unpack("<I", uc.mem_read(pc - 4, 4))[0] & 0xFFFFFF
        if imm == 3:
            ctx = uc.context_save()
            uc.reg_write(UC_ARM_REG_SP, 0x7F000)
            uc.reg_write(UC_ARM_REG_LR, SENT2)
            uc.emu_start(THUMB_FN | 1, 0xFFFFFFF0, count=1000)
            inner = uc.reg_read(UC_ARM_REG_R0)
            uc.context_restore(ctx)
            uc.reg_write(UC_ARM_REG_R0, inner + 1)
        elif imm in (1, 2):
            uc.emu_stop()
    uc.hook_add(UC_HOOK_INTR, hook2)
    try:
        r0 = run(uc, CODE)
        print(f"3. nested emu_start inside hook -> r0={r0} (expect 43)")
        ok &= r0 == 43
    except Exception as exc:
        print("3. nested emu_start FAILED:", exc)
        ok = False
    print("ALL OK" if ok else "SOME CHECKS FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
