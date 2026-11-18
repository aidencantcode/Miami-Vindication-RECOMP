"""Unicorn-based harness for libGangster2.so: memory map, import traps, guest calls.

Address map (32-bit guest):
  0x0D000000  fake Java object handles (deliberately UNMAPPED: a deref crashes loudly)
  0x0E000000  trap stubs, 8 bytes each: ARM `svc #0 ; bx lr`
  0x0E100000  bionic data objects + JNI tables
  0x10000000  libGangster2.so image
  0x30000000  HLE heap, 256 MB bump allocator (JNI copies, FILE structs, GL buffers, ...)
  0x40000000  sbrk heap (the game's own malloc grows this)
  0x60000000  mmap area
  0x7F000000  stack (16 MB, grows down from 0x80000000)

An import call goes: game -> PLT -> GOT slot -> trap stub -> `svc` -> Python
handler -> `bx lr` back to the game.
"""
import collections
import struct
import sys

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_INTR,
                     UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_PROT, UC_HOOK_MEM_READ,
                     UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC, UC_PROT_ALL,
                     UC_MEM_READ_UNMAPPED, UC_MEM_WRITE_UNMAPPED, UC_MEM_FETCH_UNMAPPED,
                     UC_MEM_WRITE_PROT, UC_MEM_FETCH_PROT, UC_MEM_READ_PROT)
from unicorn import arm_const as A

import bionic
from elfimage import ElfImage, demangle

HANDLE_BASE = 0x0D000000
TRAP_BASE, TRAP_SIZE = 0x0E000000, 0x00020000
DATA_BASE, DATA_SIZE = 0x0E100000, 0x00100000
LIB_BASE = 0x10000000
HLE_HEAP_BASE, HLE_HEAP_SIZE = 0x30000000, 0x10000000
SBRK_BASE, SBRK_LIMIT = 0x40000000, 0x58000000
MMAP_BASE, MMAP_LIMIT = 0x60000000, 0x70000000
STACK_BASE, STACK_SIZE = 0x7F000000, 0x01000000
STACK_TOP = STACK_BASE + STACK_SIZE
UNTIL = 0xFFFFFFF0
PAGE = 0x1000

SVC0, BX_LR = 0xEF000000, 0xE12FFF1E
REGS = [A.UC_ARM_REG_R0, A.UC_ARM_REG_R1, A.UC_ARM_REG_R2, A.UC_ARM_REG_R3]
ALL_REGS = [("r%d" % i, getattr(A, "UC_ARM_REG_R%d" % i)) for i in range(13)] + [
    ("sp", A.UC_ARM_REG_SP), ("lr", A.UC_ARM_REG_LR), ("pc", A.UC_ARM_REG_PC),
    ("cpsr", A.UC_ARM_REG_CPSR)]


def page_up(x):
    return (x + PAGE - 1) & ~(PAGE - 1)


class ProbeError(Exception):
    pass


class GuestExit(Exception):
    pass


class Redirect:
    """Handler result: continue execution at `addr` with the current registers
    (LR untouched, so the target returns straight to the original caller)."""
    def __init__(self, addr):
        self.addr = addr


class Args:
    """Sequential AAPCS (soft-float) argument reader: r0-r3 then stack, with
    64-bit values in even register pairs / 8-byte-aligned stack slots."""
    def __init__(self, emu, ncrn=0):
        self.emu = emu
        self.regs = [emu.uc.reg_read(r) for r in REGS]
        self.ncrn = ncrn
        self.nsaa = emu.uc.reg_read(A.UC_ARM_REG_SP)

    def u32(self):
        if self.ncrn < 4:
            v = self.regs[self.ncrn]
            self.ncrn += 1
            return v
        v = self.emu.r32(self.nsaa)
        self.nsaa += 4
        return v

    def s32(self):
        v = self.u32()
        return v - (1 << 32) if v & 0x80000000 else v

    def u64(self):
        if self.ncrn < 4 and self.ncrn & 1:
            self.ncrn += 1
        if self.ncrn <= 2:
            lo, hi = self.regs[self.ncrn], self.regs[self.ncrn + 1]
            self.ncrn += 2
        else:
            self.ncrn = 4
            self.nsaa = (self.nsaa + 7) & ~7
            lo, hi = self.emu.r32(self.nsaa), self.emu.r32(self.nsaa + 4)
            self.nsaa += 8
        return lo | (hi << 32)

    def s64(self):
        v = self.u64()
        return v - (1 << 64) if v >> 63 else v

    def f64(self):
        return struct.unpack("<d", struct.pack("<Q", self.u64()))[0]

    def raw(self, n=4):
        return [self.regs[i] for i in range(n)]


class VaList(Args):
    """Reader over an AAPCS va_list (a plain pointer to the argument area)."""
    def __init__(self, emu, ptr):
        self.emu = emu
        self.ncrn = 4
        self.nsaa = ptr


class Emu:
    def __init__(self, so_path, log=print, stub_log_limit=3):
        self.log = log
        self.stub_log_limit = stub_log_limit
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.ctl_set_cpu_model(A.UC_CPU_ARM_926)  # ARMv5TE, what the game was built for

        uc.mem_map(TRAP_BASE, TRAP_SIZE, UC_PROT_READ | UC_PROT_EXEC)
        uc.mem_map(DATA_BASE, DATA_SIZE, UC_PROT_READ | UC_PROT_WRITE)
        uc.mem_map(HLE_HEAP_BASE, HLE_HEAP_SIZE, UC_PROT_READ | UC_PROT_WRITE)
        uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_READ | UC_PROT_WRITE)
        self._data_next = DATA_BASE
        self._heap_next = HLE_HEAP_BASE
        self.brk = SBRK_BASE
        self._brk_mapped = SBRK_BASE
        self._mmap_next = MMAP_BASE

        self.traps = []              # index -> (name, handler)
        self.trap_addr = {}          # name -> address
        self.calls = collections.Counter()
        self.call_order = []         # first-call order of trap names
        self.handlers = {}           # import name -> handler (filled by HLE modules)
        self.default_handler = None  # handler for imports without one
        self.watches = []            # (name, lo, hi, reads list)
        self.depth = 0
        self.last_fault = None

        self.sentinel = self.add_trap("<return-to-host>", self._sentinel)
        self.lib = ElfImage(so_path, LIB_BASE)
        self.data = bionic.build(self.data_alloc, self.uc.mem_write)
        self.unresolved = []

        uc.hook_add(UC_HOOK_INTR, self._on_intr)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED | UC_HOOK_MEM_PROT, self._on_bad_mem)

    # -- setup ---------------------------------------------------------------
    def load(self):
        """Relocate the library against our traps/data and map it."""
        self.reloc_stats = self.lib.relocate(self.resolve_import)
        size = self.lib.size
        self.uc.mem_map(LIB_BASE, size, UC_PROT_ALL)
        self.uc.mem_write(LIB_BASE, bytes(self.lib.image))
        # Apply segment permissions (page granular).
        for vaddr, memsz, flags in self.lib.segments:
            lo = vaddr & ~(PAGE - 1)
            hi = page_up(vaddr + memsz)
            prot = ((UC_PROT_READ if "R" in flags else 0) | (UC_PROT_WRITE if "W" in flags else 0)
                    | (UC_PROT_EXEC if "X" in flags else 0))
            self.uc.mem_protect(LIB_BASE + lo, hi - lo, prot)
        return self.reloc_stats

    def resolve_import(self, sym):
        name = sym.name
        if name in bionic.DATA_IMPORTS:
            return self.data[name]
        if sym["st_info"]["type"] == "STT_OBJECT":
            self.unresolved.append(name)
            addr = self.data_alloc(64, 8)
            self.data[name] = addr
            return addr
        if name in self.trap_addr:
            return self.trap_addr[name]
        return self.add_trap(name, None)

    def add_trap(self, name, handler):
        idx = len(self.traps)
        addr = TRAP_BASE + 8 * idx
        if addr + 8 > TRAP_BASE + TRAP_SIZE:
            raise ProbeError("trap region full")
        self.uc.mem_write(addr, struct.pack("<II", SVC0, BX_LR))
        self.traps.append((name, handler))
        self.trap_addr[name] = addr
        return addr

    def data_alloc(self, size, align=4):
        a = (self._data_next + align - 1) & ~(align - 1)
        self._data_next = a + size
        if self._data_next > DATA_BASE + DATA_SIZE:
            raise ProbeError("data region full")
        return a

    def heap_alloc(self, size, align=8):
        a = (self._heap_next + align - 1) & ~(align - 1)
        self._heap_next = a + max(size, 1)
        if self._heap_next > HLE_HEAP_BASE + HLE_HEAP_SIZE:
            raise ProbeError("HLE heap full")
        return a

    def sbrk(self, incr):
        old = self.brk
        new = old + incr
        if new < SBRK_BASE or new > SBRK_LIMIT:
            return None
        if new > self._brk_mapped:
            top = page_up(new)
            self.uc.mem_map(self._brk_mapped, top - self._brk_mapped, UC_PROT_READ | UC_PROT_WRITE)
            self._brk_mapped = top
        self.brk = new
        return old

    def mmap_anon(self, length, prot=UC_PROT_READ | UC_PROT_WRITE):
        length = page_up(length)
        a = self._mmap_next
        if a + length > MMAP_LIMIT:
            return None
        self.uc.mem_map(a, length, prot)
        self._mmap_next = a + length
        return a

    def watch(self, name, addr, size):
        """Record every guest read of [addr, addr+size)."""
        reads = []
        self.watches.append((name, addr, addr + size, reads))

        def hook(uc, access, address, sz, value, _):
            reads.append((address - addr, sz, uc.reg_read(A.UC_ARM_REG_PC)))
        self.uc.hook_add(UC_HOOK_MEM_READ, hook, begin=addr, end=addr + size - 1)
        return reads

    # -- memory helpers -------------------------------------------------------
    def r32(self, a):
        return struct.unpack("<I", self.uc.mem_read(a, 4))[0]

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))

    def read(self, a, n):
        return bytes(self.uc.mem_read(a, n))

    def write(self, a, data):
        self.uc.mem_write(a, bytes(data))

    def cstr(self, a, limit=1 << 20):
        if a == 0:
            return None
        out = bytearray()
        while len(out) < limit:
            chunk = self.uc.mem_read(a + len(out), 256 - ((a + len(out)) & 0xFF))
            i = chunk.find(0)
            if i >= 0:
                out += chunk[:i]
                return bytes(out)
            out += chunk
        raise ProbeError(f"unterminated string at {a:#x}")

    def wstr(self, a, limit=1 << 18):
        if a == 0:
            return None
        chars = []
        while len(chars) < limit:
            c = self.r32(a + 4 * len(chars))
            if c == 0:
                return "".join(chars)
            chars.append(chr(c) if c < 0x110000 else "�")
        raise ProbeError(f"unterminated wide string at {a:#x}")

    # -- symbols / diagnostics ------------------------------------------------
    def where(self, addr):
        s = self.lib.sym_at(addr)
        if s:
            return s
        if TRAP_BASE <= addr < TRAP_BASE + TRAP_SIZE:
            idx = (addr - TRAP_BASE) // 8
            if idx < len(self.traps):
                return f"<trap {self.traps[idx][0]}>"
        return f"{addr:#010x}"

    def regs_dump(self):
        vals = {n: self.uc.reg_read(r) for n, r in ALL_REGS}
        lines = ["  " + "  ".join(f"{n}={vals[n]:08x}" for n, _ in ALL_REGS[i:i + 4])
                 for i in range(0, len(ALL_REGS), 4)]
        lines.append(f"  pc in {self.where(vals['pc'])}   lr in {self.where(vals['lr'])}"
                     f"   mode={'Thumb' if vals['cpsr'] & 0x20 else 'ARM'}")
        return "\n".join(lines)

    def backtrace_guess(self, depth=12):
        """Scan the stack for words that look like return addresses into the lib."""
        sp = self.uc.reg_read(A.UC_ARM_REG_SP)
        out = []
        for i in range(2048):
            a = sp + 4 * i
            if a >= STACK_TOP:
                break
            v = self.r32(a)
            if LIB_BASE <= v < LIB_BASE + self.lib.size and v & 1:
                rel = (v & ~1) - LIB_BASE
                info = self.lib.func_start(rel)
                s = self.lib.sym_at(v)
                if s and info is None:  # inside a function, not at its start
                    out.append(f"  [sp+{4*i:#x}] {s}")
                    if len(out) >= depth:
                        break
        return "\n".join(out)

    # -- hooks ---------------------------------------------------------------
    def _on_bad_mem(self, uc, access, address, size, value, _):
        kinds = {UC_MEM_READ_UNMAPPED: "read unmapped", UC_MEM_WRITE_UNMAPPED: "write unmapped",
                 UC_MEM_FETCH_UNMAPPED: "fetch unmapped", UC_MEM_WRITE_PROT: "write to read-only",
                 UC_MEM_FETCH_PROT: "fetch from non-exec", UC_MEM_READ_PROT: "read protected"}
        what = "Java object handle (opaque!)" if HANDLE_BASE <= address < TRAP_BASE else ""
        self.last_fault = f"{kinds.get(access, access)} at {address:#010x} size {size} {what}"
        return False

    def _sentinel(self, emu, name, args):
        self._returned = True
        self.uc.emu_stop()
        return None

    def _on_intr(self, uc, intno, _):
        pc = uc.reg_read(A.UC_ARM_REG_PC)
        svc_addr = pc - 4
        if intno != 2 or not TRAP_BASE <= svc_addr < TRAP_BASE + len(self.traps) * 8:
            self.last_fault = f"unexpected interrupt {intno} at {svc_addr:#x}"
            uc.emu_stop()
            return
        name, handler = self.traps[(svc_addr - TRAP_BASE) // 8]
        if handler is None:
            handler = self.handlers.get(name, self.default_handler)
        if name != "<return-to-host>":
            if name not in self.calls:
                self.call_order.append(name)
            self.calls[name] += 1
        res = handler(self, name, Args(self))
        if res is None:
            return
        if isinstance(res, Redirect):
            uc.reg_write(A.UC_ARM_REG_PC, res.addr)
            return
        if isinstance(res, int):
            res = (res,)
        for reg, v in zip(REGS, res):
            uc.reg_write(reg, v & 0xFFFFFFFF)

    # -- calling guest code -----------------------------------------------------
    def call(self, addr, *args, stack_args=(), count=2_000_000_000):
        """Call guest function `addr` (Thumb bit honoured) with up to 4 register
        args + stack args. Works from inside a handler (nested). Returns (r0, r1)."""
        uc = self.uc
        nested = self.depth > 0
        ctx = uc.context_save() if nested else None
        sp = (uc.reg_read(A.UC_ARM_REG_SP) - 256) & ~7 if nested else STACK_TOP - 64
        if stack_args:
            sp -= (4 * len(stack_args) + 7) & ~7
            for i, v in enumerate(stack_args):
                self.w32(sp + 4 * i, v)
        uc.reg_write(A.UC_ARM_REG_SP, sp)
        for reg, v in zip(REGS, list(args) + [0] * (4 - len(args))):
            uc.reg_write(reg, v & 0xFFFFFFFF)
        uc.reg_write(A.UC_ARM_REG_LR, self.sentinel)
        self.depth += 1
        self._returned = False
        try:
            uc.emu_start(addr, UNTIL, count=count)
        except UcError as exc:
            msg = [f"emulation error: {exc}", f"  fault: {self.last_fault}", self.regs_dump()]
            bt = self.backtrace_guess()
            if bt:
                msg += ["  stack return-address guesses:", bt]
            raise ProbeError("\n".join(msg)) from None
        finally:
            self.depth -= 1
        returned, self._returned = self._returned, False
        r0, r1 = uc.reg_read(A.UC_ARM_REG_R0), uc.reg_read(A.UC_ARM_REG_R1)
        if not returned:
            why = self.last_fault or "instruction limit reached or emu_stop from a handler"
            raise ProbeError(f"guest call to {self.where(addr)} did not return: {why}\n{self.regs_dump()}")
        if nested:
            uc.context_restore(ctx)
        return r0, r1

    def func(self, name):
        if name in self.lib.funcs:
            return LIB_BASE + self.lib.funcs[name]
        raise KeyError(name)
