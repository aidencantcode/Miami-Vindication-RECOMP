"""Unit tests for pure HLE helpers (soft-float, integer division, strtol, printf).
Run:  python -I tools/probe/test_hle.py
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hle_libc as L  # noqa: E402


class FakeArgs:
    def __init__(self, *vals):
        self.vals = list(vals)

    def _next(self):
        return self.vals.pop(0)

    def u32(self): return self._next() & 0xFFFFFFFF
    def s32(self): return L.s32(self._next())
    def u64(self): return self._next() & 0xFFFFFFFFFFFFFFFF
    def s64(self): return self._next()
    def f64(self): return float(self._next())


class FakeEmu:
    def __init__(self, strings):
        self.strings = strings

    def cstr(self, a):
        return self.strings.get(a)


def fbits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


fails = 0


def eq(got, want, what):
    global fails
    ok = got == want or (isinstance(want, float) and math.isnan(want) and math.isnan(got))
    print(("ok    " if ok else "FAIL  ") + f"{what}: got {got!r}, want {want!r}")
    fails += not ok


# soft-float
eq(L._fadd(None, "", FakeArgs(0x3DCCCCCD, 0x3E4CCCCD)), 0x3E99999A, "0.1f + 0.2f bits")
eq(L._fmul(None, "", FakeArgs(fbits(3.0), fbits(1 / 3))), fbits(1.0), "3f * (1/3)f")
eq(L._fdiv(None, "", FakeArgs(fbits(1.0), fbits(0.0))), 0x7F800000, "1f / 0f = +inf")
eq(L._fdiv(None, "", FakeArgs(fbits(-1.0), fbits(0.0))), 0xFF800000, "-1f / 0f = -inf")
eq(L._fdiv(None, "", FakeArgs(fbits(0.0), fbits(0.0))) & 0x7F800000, 0x7F800000, "0f / 0f = NaN (exponent all ones)")
eq(L.b32(3.4028236e38), 0x7F800000, "double above FLT_MAX rounds to +inf")
eq(L.int_to_f32(16777217), 16777216.0, "int->float tie rounds to even (2^24+1)")
eq(L.int_to_f32(16777219), 16777220.0, "int->float tie rounds to even (2^24+3)")
eq(L._l2f(None, "", FakeArgs(2**63 - 1)), 0x5F000000, "__aeabi_l2f(INT64_MAX)")
eq(L._f2iz(None, "", FakeArgs(fbits(-1.5))), 0xFFFFFFFF, "__aeabi_f2iz(-1.5f) = -1")
eq(L._f2iz(None, "", FakeArgs(fbits(1e10))), 0x7FFFFFFF, "__aeabi_f2iz saturates")
eq(L._f2iz(None, "", FakeArgs(0x7FC00000)), 0, "__aeabi_f2iz(NaN) = 0")
eq(L._d2uiz(None, "", FakeArgs(-1.0)), 0, "__aeabi_d2uiz(-1.0) = 0")
eq(L._fcmplt(None, "", FakeArgs(0x7FC00000, fbits(1.0))), 0, "NaN < 1 is false")
eq(L._dcmpun(None, "", FakeArgs(math.nan, 1.0)), 1, "__aeabi_dcmpun(NaN, 1)")
eq(L._d2f(None, "", FakeArgs(0.1)), 0x3DCCCCCD, "__aeabi_d2f(0.1)")
eq(L._ddiv(None, "", FakeArgs(1.0, 0.0)), L.b64(math.inf), "__aeabi_ddiv(1, 0) = inf")

# integer division
eq(L._idivmod(None, "", FakeArgs(-7, 2)), (L.u32(-3), L.u32(-1)), "__aeabi_idivmod(-7, 2) truncates")
eq(L._uidivmod(None, "", FakeArgs(7, 2)), (3, 1), "__aeabi_uidivmod(7, 2)")
eq(L._ldivmod(None, "", FakeArgs(-(2**40) - 1, 2**20)), L.split64(-(2**20)) + L.split64(-1), "__aeabi_ldivmod 64-bit")
eq(L._lmul(None, "", FakeArgs(2**33 + 1, 2**33 + 1)), L.split64((2**33 + 1) ** 2), "__aeabi_lmul wraps to 64 bits")

# strtol parsing
for s, base, want in ((b"  -0x1Fz", 0, (-31, 7)), (b"0755", 0, (493, 4)), (b"12abc", 10, (12, 2)),
                      (b"abc", 10, (0, 0)), (b"0x", 16, (0, 1)), (b"+5", 0, (5, 2)), (b"ff", 16, (255, 2)),
                      (b"0xg", 0, (0, 1))):
    eq(L._strtoi(s, base), want, f"strtol({s!r}, base {base})")

# printf formatting
emu = FakeEmu({0x100: b"hello", 0x200: b"%d|%5.2f|%s|%x|%-3c|%08.3f|%lld|%u|%%|%#x|%p|%.2s|%05d"})
got = L.cformat(emu, emu.cstr(0x200),
                FakeArgs(-5, 3.14159, 0x100, 255, ord("A"), 2.5, 2**40, 0xFFFFFFFF, 0, 0x1234, 0x100, -42))
eq(got, b"-5| 3.14|hello|ff|A  |0002.500|1099511627776|4294967295|%|0|0x1234|he|-0042", "cformat mix")

# sscanf
class MemEmu:
    def __init__(self):
        self.mem = {}

    def write(self, a, data):
        self.mem[a] = bytes(data)

    def w32(self, a, v):
        self.mem[a] = struct.pack("<I", v & 0xFFFFFFFF)


def scan(s, fmt, nptr):
    m = MemEmu()
    ptrs = [0x1000 * (i + 1) for i in range(nptr)]
    r = L.csscanf(m, s, fmt, FakeArgs(*ptrs))
    return L.s32(r), [m.mem.get(p) for p in ptrs]


r, v = scan(b"1.1", b"%d.%d", 2)
eq((r, struct.unpack("<i", v[0])[0], struct.unpack("<i", v[1])[0]), (2, 1, 1), "sscanf('1.1', '%d.%d')  (GL version parse)")
r, v = scan(b"  42 abc 3.5", b"%d %s %f", 3)
eq((r, struct.unpack("<i", v[0])[0], v[1], round(struct.unpack("<f", v[2])[0], 3)), (3, 42, b"abc\0", 3.5), "sscanf mixed %d %s %f")
r, v = scan(b"x=10,y=0x1f", b"x=%d,y=%x", 2)
eq((r, struct.unpack("<i", v[0])[0], struct.unpack("<i", v[1])[0]), (2, 10, 31), "sscanf literals + hex")
eq(scan(b"", b"%d", 1)[0], -1, "sscanf on empty input = EOF (-1)")
eq(scan(b"abc", b"%d", 1)[0], 0, "sscanf no match = 0")
r, v = scan(b"ab12", b"%[a-z]%n", 2)
eq((r, v[0], struct.unpack("<I", v[1])[0]), (1, b"ab\0", 2), "sscanf scanset + %n")
r, v = scan(b"2.50", b"%lf", 1)
eq((r, struct.unpack("<d", v[0])[0]), (1, 2.5), "sscanf %lf writes a double")

# path normalisation (Android process cwd is '/')
fs = L.FS(None, "sandbox")
for g, want in (("./sdcard/gameloft/games/Gangstar2//./about.english", "/sdcard/gameloft/games/Gangstar2/about.english"),
                ("/sdcard//a/../b.txt", "/sdcard/b.txt"), ("rel.txt", "/rel.txt")):
    eq(fs.normalize(g), want, f"normalize({g!r})")

print("ALL OK" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
