"""Host-side (HLE) implementations of libGangster2's bionic imports, for the probe.

Handler signature: fn(emu, name, args) -> None | int | (r0, r1[, r2, r3]) | Redirect.
Anything without a handler goes to `default_stub`, which logs `STUB: name(...)`
and returns 0, so unimplemented calls are never silent.

Layouts used (bionic, 32-bit ARM, Android 2.x headers; see NOTES Q10):
  FILE 84 bytes (_flags @12, _file @14); struct stat 104 bytes (st_mode @16,
  st_size @48 as int64, st_mtime @80); struct tm 44 bytes; struct timeval 8 bytes;
  struct dirent: d_ino u64 @0, d_off s64 @8, d_reclen u16 @16, d_type u8 @18,
  d_name @19.
File system: guest paths under /sdcard/gameloft/games/Gangstar2/ map to the
owner's data folder READ-ONLY; every write goes to the sandbox overlay.
"""
import ctypes
import functools
import math
import os
import re
import struct
import time

from emu import Redirect, ProbeError, GuestExit, VaList

H = {}


def hle(*names):
    def deco(fn):
        for n in names:
            H[n] = fn
        return fn
    return deco


def default_stub(emu, name, a):
    emu.stubbed.add(name)
    if emu.calls[name] <= emu.stub_log_limit:
        r = a.raw()
        lr = emu.uc.reg_read(emu_lr())
        emu.log(f"STUB: {name}({r[0]:#x}, {r[1]:#x}, {r[2]:#x}, {r[3]:#x}) -> 0   [from {emu.where(lr)}]")
    return 0


def emu_lr():
    from unicorn.arm_const import UC_ARM_REG_LR
    return UC_ARM_REG_LR


def u32(x):
    return x & 0xFFFFFFFF


def s32(x):
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x & 0x80000000 else x


def split64(v):
    v &= 0xFFFFFFFFFFFFFFFF
    return v & 0xFFFFFFFF, v >> 32


# ---------------------------------------------------------------------------
# soft-float runtime (__aeabi_*). Computing float ops in double and rounding
# once to float32 is correctly rounded for + - * / (53 >= 2*24+2).
def f32(bits):
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def b32(x):
    return struct.unpack("<I", struct.pack("<f", ctypes.c_float(x).value))[0]


def b64(x):
    return split64(struct.unpack("<Q", struct.pack("<d", x))[0])


def fdiv(a, b):
    try:
        return a / b
    except ZeroDivisionError:
        if a != a or a == 0:
            return math.nan
        return math.copysign(math.inf, a) * math.copysign(1.0, b)


def int_to_f32(v):
    """Correctly rounded int64 -> float32 (avoids double rounding)."""
    if v == 0:
        return 0.0
    sign = -1.0 if v < 0 else 1.0
    a = abs(v)
    n = a.bit_length()
    if n <= 24:
        return float(v)
    shift = n - 24
    m, rem, half = a >> shift, a & ((1 << shift) - 1), 1 << (shift - 1)
    if rem > half or (rem == half and m & 1):
        m += 1
    return sign * float(m << shift)


def f_to_int(x, lo, hi):
    if x != x:
        return 0
    if x >= hi:
        return hi
    if x <= lo:
        return lo
    return int(x)  # truncates toward zero


@hle("__aeabi_fadd")
def _fadd(e, n, a): return b32(f32(a.u32()) + f32(a.u32()))
@hle("__aeabi_fsub")
def _fsub(e, n, a): return b32(f32(a.u32()) - f32(a.u32()))
@hle("__aeabi_fmul")
def _fmul(e, n, a): return b32(f32(a.u32()) * f32(a.u32()))
@hle("__aeabi_fdiv")
def _fdiv(e, n, a): return b32(fdiv(f32(a.u32()), f32(a.u32())))
@hle("__aeabi_fcmpeq")
def _fcmpeq(e, n, a): x, y = f32(a.u32()), f32(a.u32()); return int(x == y)
@hle("__aeabi_fcmplt")
def _fcmplt(e, n, a): x, y = f32(a.u32()), f32(a.u32()); return int(x < y)
@hle("__aeabi_fcmple")
def _fcmple(e, n, a): x, y = f32(a.u32()), f32(a.u32()); return int(x <= y)
@hle("__aeabi_fcmpge")
def _fcmpge(e, n, a): x, y = f32(a.u32()), f32(a.u32()); return int(x >= y)
@hle("__aeabi_fcmpgt")
def _fcmpgt(e, n, a): x, y = f32(a.u32()), f32(a.u32()); return int(x > y)
@hle("__aeabi_f2iz")
def _f2iz(e, n, a): return u32(f_to_int(f32(a.u32()), -2**31, 2**31 - 1))
@hle("__aeabi_f2lz")
def _f2lz(e, n, a): return split64(f_to_int(f32(a.u32()), -2**63, 2**63 - 1))
@hle("__aeabi_f2d")
def _f2d(e, n, a): return b64(f32(a.u32()))
@hle("__aeabi_i2f")
def _i2f(e, n, a): return b32(float(a.s32()))
@hle("__aeabi_ui2f")
def _ui2f(e, n, a): return b32(int_to_f32(a.u32()))
@hle("__aeabi_l2f")
def _l2f(e, n, a): return b32(int_to_f32(a.s64()))
@hle("__aeabi_dadd")
def _dadd(e, n, a): return b64(a.f64() + a.f64())
@hle("__aeabi_dsub")
def _dsub(e, n, a): return b64(a.f64() - a.f64())
@hle("__aeabi_dmul")
def _dmul(e, n, a): return b64(a.f64() * a.f64())
@hle("__aeabi_ddiv")
def _ddiv(e, n, a): return b64(fdiv(a.f64(), a.f64()))
@hle("__aeabi_dcmpeq")
def _dcmpeq(e, n, a): x, y = a.f64(), a.f64(); return int(x == y)
@hle("__aeabi_dcmplt")
def _dcmplt(e, n, a): x, y = a.f64(), a.f64(); return int(x < y)
@hle("__aeabi_dcmple")
def _dcmple(e, n, a): x, y = a.f64(), a.f64(); return int(x <= y)
@hle("__aeabi_dcmpge")
def _dcmpge(e, n, a): x, y = a.f64(), a.f64(); return int(x >= y)
@hle("__aeabi_dcmpgt")
def _dcmpgt(e, n, a): x, y = a.f64(), a.f64(); return int(x > y)
@hle("__aeabi_dcmpun")
def _dcmpun(e, n, a): x, y = a.f64(), a.f64(); return int(x != x or y != y)
@hle("__aeabi_d2f")
def _d2f(e, n, a): return b32(a.f64())
@hle("__aeabi_d2iz")
def _d2iz(e, n, a): return u32(f_to_int(a.f64(), -2**31, 2**31 - 1))
@hle("__aeabi_d2uiz")
def _d2uiz(e, n, a): return f_to_int(a.f64(), 0, 2**32 - 1)
@hle("__aeabi_d2lz")
def _d2lz(e, n, a): return split64(f_to_int(a.f64(), -2**63, 2**63 - 1))
@hle("__aeabi_i2d")
def _i2d(e, n, a): return b64(float(a.s32()))
@hle("__aeabi_ui2d")
def _ui2d(e, n, a): return b64(float(a.u32()))
@hle("__aeabi_ul2d")
def _ul2d(e, n, a): return b64(float(a.u64()))


def _tdiv(x, y):
    q = abs(x) // abs(y)
    return q if (x < 0) == (y < 0) else -q


def _div0(e, name):
    e.log(f"WARNING: {name}: integer division by zero (returning 0) [from {e.where(e.uc.reg_read(emu_lr()))}]")


@hle("__aeabi_idiv")
def _idiv(e, n, a):
    x, y = a.s32(), a.s32()
    if y == 0:
        _div0(e, n); return 0
    return u32(_tdiv(x, y))


@hle("__aeabi_idivmod")
def _idivmod(e, n, a):
    x, y = a.s32(), a.s32()
    if y == 0:
        _div0(e, n); return (0, 0)
    q = _tdiv(x, y)
    return (u32(q), u32(x - q * y))


@hle("__aeabi_uidiv")
def _uidiv(e, n, a):
    x, y = a.u32(), a.u32()
    if y == 0:
        _div0(e, n); return 0
    return x // y


@hle("__aeabi_uidivmod")
def _uidivmod(e, n, a):
    x, y = a.u32(), a.u32()
    if y == 0:
        _div0(e, n); return (0, 0)
    return (x // y, x % y)


@hle("__aeabi_ldivmod")
def _ldivmod(e, n, a):
    x, y = a.s64(), a.s64()
    if y == 0:
        _div0(e, n); return (0, 0, 0, 0)
    q = _tdiv(x, y)
    return split64(q) + split64(x - q * y)


@hle("__aeabi_uldivmod")
def _uldivmod(e, n, a):
    x, y = a.u64(), a.u64()
    if y == 0:
        _div0(e, n); return (0, 0, 0, 0)
    return split64(x // y) + split64(x % y)


@hle("__aeabi_lmul")
def _lmul(e, n, a): return split64(a.u64() * a.u64())


# ---------------------------------------------------------------------------
# libm (approximate: host math, rounded to float for the *f variants)
def _safe(fn, *xs):
    try:
        return fn(*xs)
    except OverflowError:
        return math.inf
    except (ValueError, ZeroDivisionError):
        if fn in (math.log, math.log10) and xs[0] == 0:
            return -math.inf
        if fn is math.pow and xs[0] == 0 and xs[1] < 0:
            return math.inf
        return math.nan


def _mf1(fn):
    return lambda e, n, a: b32(_safe(fn, f32(a.u32())))


def _md1(fn):
    return lambda e, n, a: b64(_safe(fn, a.f64()))


for _name, _fn in (("acos", math.acos), ("asin", math.asin), ("atan", math.atan), ("ceil", math.ceil),
                   ("cos", math.cos), ("exp", math.exp), ("floor", math.floor), ("log", math.log),
                   ("log10", math.log10), ("sin", math.sin), ("sqrt", math.sqrt), ("tan", math.tan)):
    _f = (lambda fn: (lambda x: float(fn(x)) if x == x and abs(x) != math.inf else x)
          if fn in (math.ceil, math.floor) else fn)(_fn)
    H[_name] = _md1(_f)
    H[_name + "f"] = _mf1(_f)
H.pop("log10f", None)  # not imported; keep the table honest
H["atan2"] = lambda e, n, a: b64(_safe(math.atan2, a.f64(), a.f64()))
H["atan2f"] = lambda e, n, a: b32(_safe(math.atan2, f32(a.u32()), f32(a.u32())))
H["pow"] = lambda e, n, a: b64(_safe(math.pow, a.f64(), a.f64()))
H["powf"] = lambda e, n, a: b32(_safe(math.pow, f32(a.u32()), f32(a.u32())))
H["fmod"] = lambda e, n, a: b64(_safe(math.fmod, a.f64(), a.f64()))
H["fmodf"] = lambda e, n, a: b32(_safe(math.fmod, f32(a.u32()), f32(a.u32())))
H["ldexpf"] = lambda e, n, a: b32(_safe(math.ldexp, f32(a.u32()), a.s32()))
H["__isfinitef"] = lambda e, n, a: int(math.isfinite(f32(a.u32())))


# ---------------------------------------------------------------------------
# memory and strings
def game_malloc(e, size):
    """Allocate with the GAME's own malloc (it defines malloc/free itself), so
    the game can later free() the result."""
    return e.call(e.func("malloc"), size)[0]


@hle("memcpy", "memmove")
def _memcpy(e, n, a):
    d, s, ln = a.u32(), a.u32(), a.u32()
    if ln:
        e.write(d, e.read(s, ln))
    return d


@hle("memset")
def _memset(e, n, a):
    d, c, ln = a.u32(), a.u32() & 0xFF, a.u32()
    if ln:
        e.write(d, bytes([c]) * ln)
    return d


@hle("memcmp")
def _memcmp(e, n, a):
    x, y, ln = a.u32(), a.u32(), a.u32()
    p, q = e.read(x, ln), e.read(y, ln)
    for i in range(ln):
        if p[i] != q[i]:
            return u32(p[i] - q[i])
    return 0


@hle("memchr")
def _memchr(e, n, a):
    s, c, ln = a.u32(), a.u32() & 0xFF, a.u32()
    i = e.read(s, ln).find(bytes([c])) if ln else -1
    return s + i if i >= 0 else 0


@hle("strlen")
def _strlen(e, n, a): return len(e.cstr(a.u32()))


def _cmp(x, y):
    for i in range(min(len(x), len(y))):
        if x[i] != y[i]:
            return u32(x[i] - y[i])
    return u32(len(x) - len(y)) if len(x) != len(y) else 0


def _cmp_bytes(x, y):
    """C comparison of two NUL-terminated strings given without the NUL."""
    return _cmp(x + b"\0", y + b"\0")


@hle("strcmp")
def _strcmp(e, n, a): return _cmp_bytes(e.cstr(a.u32()), e.cstr(a.u32()))


@hle("strncmp")
def _strncmp(e, n, a):
    x, y, ln = e.cstr(a.u32()), e.cstr(a.u32()), a.u32()
    return _cmp_bytes(x[:ln], y[:ln])


@hle("strcasecmp")
def _strcasecmp(e, n, a): return _cmp_bytes(e.cstr(a.u32()).lower(), e.cstr(a.u32()).lower())


@hle("strncasecmp")
def _strncasecmp(e, n, a):
    x, y, ln = e.cstr(a.u32()), e.cstr(a.u32()), a.u32()
    return _cmp_bytes(x[:ln].lower(), y[:ln].lower())


@hle("strcpy")
def _strcpy(e, n, a):
    d, s = a.u32(), a.u32()
    e.write(d, e.cstr(s) + b"\0")
    return d


@hle("strncpy")
def _strncpy(e, n, a):
    d, s, ln = a.u32(), a.u32(), a.u32()
    src = e.cstr(s)[:ln]
    e.write(d, src + b"\0" * (ln - len(src)))
    return d


@hle("strcat")
def _strcat(e, n, a):
    d, s = a.u32(), a.u32()
    e.write(d + len(e.cstr(d)), e.cstr(s) + b"\0")
    return d


@hle("strncat")
def _strncat(e, n, a):
    d, s, ln = a.u32(), a.u32(), a.u32()
    e.write(d + len(e.cstr(d)), e.cstr(s)[:ln] + b"\0")
    return d


@hle("strchr")
def _strchr(e, n, a):
    s, c = a.u32(), a.u32() & 0xFF
    t = e.cstr(s) + b"\0"
    i = t.find(bytes([c]))
    return s + i if i >= 0 else 0


@hle("strrchr")
def _strrchr(e, n, a):
    s, c = a.u32(), a.u32() & 0xFF
    t = e.cstr(s) + b"\0"
    i = t.rfind(bytes([c]))
    return s + i if i >= 0 else 0


@hle("strstr")
def _strstr(e, n, a):
    s, sub = a.u32(), a.u32()
    i = e.cstr(s).find(e.cstr(sub))
    return s + i if i >= 0 else 0


@hle("strcspn")
def _strcspn(e, n, a):
    s, rej = e.cstr(a.u32()), set(e.cstr(a.u32()))
    for i, ch in enumerate(s):
        if ch in rej:
            return i
    return len(s)


@hle("strdup")
def _strdup(e, n, a):
    s = e.cstr(a.u32())
    p = game_malloc(e, len(s) + 1)
    if p:
        e.write(p, s + b"\0")
    return p


@hle("wcslen")
def _wcslen(e, n, a): return len(e.wstr(a.u32()))


@hle("wcscpy")
def _wcscpy(e, n, a):
    d, s = a.u32(), a.u32()
    w = e.wstr(s)
    e.write(d, struct.pack(f"<{len(w) + 1}I", *[ord(c) for c in w], 0))
    return d


@hle("wcscmp")
def _wcscmp(e, n, a):
    x, y = e.wstr(a.u32()), e.wstr(a.u32())
    return 0 if x == y else (u32(-1) if x < y else 1)


# -- number parsing ---------------------------------------------------------
def _digit(c):
    if 48 <= c <= 57:
        return c - 48
    if 97 <= c <= 122:
        return c - 87
    if 65 <= c <= 90:
        return c - 55
    return 99


def _strtoi(s, base):
    """C strtol parsing. Returns (value, number of bytes consumed); (0, 0) if no digits."""
    i, n = 0, len(s)
    while i < n and s[i] in b" \t\n\v\f\r":
        i += 1
    neg = False
    if i < n and s[i] in b"+-":
        neg = s[i] == 0x2D
        i += 1
    if base in (0, 16) and s[i:i + 2].lower() == b"0x" and i + 2 < n and _digit(s[i + 2]) < 16:
        i += 2
        base = 16
    elif base == 0:
        base = 8 if i < n and s[i] == 0x30 else 10
    start, val = i, 0
    while i < n and _digit(s[i]) < base:
        val = val * base + _digit(s[i])
        i += 1
    if i == start:
        return 0, 0
    return (-val if neg else val), i


@hle("strtol", "strtoul")
def _strtol(e, n, a):
    p, endp, base = a.u32(), a.u32(), a.s32()
    v, used = _strtoi(e.cstr(p), base)
    if endp:
        e.w32(endp, p + used)
    if n == "strtol":
        v = max(-2**31, min(2**31 - 1, v))
    return u32(v)


@hle("atoi", "atol")
def _atoi(e, n, a): return u32(_strtoi(e.cstr(a.u32()), 10)[0])


_FLT_RE = re.compile(rb"\s*([+-]?(?:inf(?:inity)?|nan|(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?))", re.I)


@hle("strtod")
def _strtod(e, n, a):
    p, endp = a.u32(), a.u32()
    m = _FLT_RE.match(e.cstr(p))
    v, used = (float(m.group(1).decode("ascii")), m.end()) if m else (0.0, 0)
    if endp:
        e.w32(endp, p + used)
    return b64(v)


# ---------------------------------------------------------------------------
# printf family
_FMT = re.compile(rb"%([-+ #0]*)(\*|\d+)?(?:\.(\*|\d*))?(hh|h|ll|l|L|q|j|z|t)?([diouxXeEfFgGaAcspn%])")


def cformat(e, fmt, args):
    out = bytearray()
    pos = 0
    for m in _FMT.finditer(fmt):
        out += fmt[pos:m.start()]
        pos = m.end()
        flags, width, prec, length, conv = m.groups()
        flags, conv = flags.decode(), conv.decode()
        if conv == "%":
            out += b"%"
            continue
        w = ""
        if width == b"*":
            wv = args.s32()
            if wv < 0:
                flags, wv = flags + "-", -wv
            w = str(wv)
        elif width:
            w = width.decode()
        p = None
        if prec is not None:
            if prec == b"*":
                pv = args.s32()
                p = str(pv) if pv >= 0 else None
            else:
                p = prec.decode() or "0"
        wide = length in (b"ll", b"q", b"j")
        if conv in "di":
            v = args.s64() if wide else args.s32()
            if length == b"h":
                v = ((v + 0x8000) & 0xFFFF) - 0x8000
            elif length == b"hh":
                v = ((v + 0x80) & 0xFF) - 0x80
            spec = "%" + flags + w + ("." + p if p is not None else "")
            out += (spec + "d").encode() % v
        elif conv in "ouxX":
            v = args.u64() if wide else args.u32()
            if length == b"h":
                v &= 0xFFFF
            elif length == b"hh":
                v &= 0xFF
            if v == 0 and conv in "xX":
                flags = flags.replace("#", "")
            spec = "%" + flags + w + ("." + p if p is not None else "")
            out += (spec + ("d" if conv == "u" else conv)).encode() % v
        elif conv in "eEfFgGaA":
            v = args.f64()
            spec = "%" + flags + w + ("." + p if p is not None else "")
            out += (float.hex(v).encode() if conv in "aA" else (spec + conv).encode() % v)
        elif conv == "c":
            spec = "%" + flags + w
            out += (spec + "c").encode() % (args.u32() & 0xFF)
        elif conv == "s":
            ptr = args.u32()
            text = (e.wstr(ptr).encode("utf-8") if ptr else None) if length == b"l" else e.cstr(ptr)
            spec = "%" + flags + w + ("." + p if p is not None else "")
            out += (spec + "s").encode() % (b"(null)" if text is None else text)
        elif conv == "p":
            out += ("%" + flags + w + "s").encode() % ("0x%x" % args.u32()).encode()
        elif conv == "n":
            args.u32()
    out += fmt[pos:]
    return bytes(out)


def _emit(e, stream, data):
    text = data.decode("utf-8", "replace").rstrip("\n")
    if text:
        for line in text.split("\n"):
            e.log(f"[{stream}] {line}")


def _store(e, buf, size, data):
    if size is None:
        e.write(buf, data + b"\0")
    elif size > 0:
        e.write(buf, data[:size - 1] + b"\0")
    return len(data)


@hle("printf")
def _printf(e, n, a):
    d = cformat(e, e.cstr(a.u32()), a); _emit(e, "stdout", d); return len(d)


@hle("vprintf")
def _vprintf(e, n, a):
    fmt, va = a.u32(), a.u32()
    d = cformat(e, e.cstr(fmt), VaList(e, va)); _emit(e, "stdout", d); return len(d)


@hle("sprintf")
def _sprintf(e, n, a):
    buf = a.u32(); return _store(e, buf, None, cformat(e, e.cstr(a.u32()), a))


@hle("snprintf")
def _snprintf(e, n, a):
    buf, size = a.u32(), a.u32(); return _store(e, buf, size, cformat(e, e.cstr(a.u32()), a))


@hle("vsprintf")
def _vsprintf(e, n, a):
    buf, fmt, va = a.u32(), a.u32(), a.u32()
    return _store(e, buf, None, cformat(e, e.cstr(fmt), VaList(e, va)))


@hle("vsnprintf")
def _vsnprintf(e, n, a):
    buf, size, fmt, va = a.u32(), a.u32(), a.u32(), a.u32()
    return _store(e, buf, size, cformat(e, e.cstr(fmt), VaList(e, va)))


@hle("swprintf")
def _swprintf(e, n, a):
    buf, size, fmt = a.u32(), a.u32(), a.u32()
    text = cformat(e, e.wstr(fmt).encode("utf-8"), a).decode("utf-8", "replace")
    w = [ord(c) for c in text][:max(size - 1, 0)] if size else []
    e.write(buf, struct.pack(f"<{len(w) + 1}I", *w, 0))
    return len(text) if len(text) < size else u32(-1)


@hle("puts")
def _puts(e, n, a): _emit(e, "stdout", e.cstr(a.u32())); return 1


@hle("putchar")
def _putchar(e, n, a):
    c = a.u32() & 0xFF; _emit(e, "stdout", bytes([c])); return c


_LOG_PRIO = {2: "V", 3: "D", 4: "I", 5: "W", 6: "E", 7: "F"}


@hle("__android_log_print")
def _alog_print(e, n, a):
    prio, tag, fmt = a.u32(), e.cstr(a.u32()), e.cstr(a.u32())
    msg = cformat(e, fmt, a)
    e.log(f"[log {_LOG_PRIO.get(prio, prio)}/{(tag or b'').decode('latin1')}] {msg.decode('utf-8', 'replace').rstrip()}")
    return 1


@hle("__android_log_write")
def _alog_write(e, n, a):
    prio, tag, text = a.u32(), e.cstr(a.u32()), e.cstr(a.u32())
    e.log(f"[log {_LOG_PRIO.get(prio, prio)}/{(tag or b'').decode('latin1')}] {(text or b'').decode('utf-8', 'replace').rstrip()}")
    return 1


# ---------------------------------------------------------------------------
# stdio + POSIX files through the read-only/sandbox overlay
class GFile:
    def __init__(self, fh, path, writable):
        self.fh, self.path, self.writable = fh, path, writable
        self.eof = False
        self.pushback = []


class FS:
    PREFIXES = ("/sdcard/gameloft/games/gangstar2", "/mnt/sdcard/gameloft/games/gangstar2",
                "/storage/emulated/0/gameloft/games/gangstar2", "/sdcard/gameloft/games/gangster2")
    SAVE_PREFIX = "/data/data/com.gameloft.android.gand.gloftgmhp.ml"

    def __init__(self, data_dir, sandbox):
        self.data_dir, self.sandbox = data_dir, sandbox
        self.index = {f.lower(): f for f in os.listdir(data_dir)} if data_dir and os.path.isdir(data_dir) else {}

    def map(self, guest):
        """-> (readonly_host_path or None, sandbox_host_path)"""
        g = guest.replace("\\", "/")
        low = g.lower()
        for pre in self.PREFIXES:
            if low == pre or low.startswith(pre + "/"):
                rest = g[len(pre):].lstrip("/")
                ro = None
                if rest and "/" not in rest and rest.lower() in self.index:
                    ro = os.path.join(self.data_dir, self.index[rest.lower()])
                elif not rest:
                    ro = self.data_dir
                return ro, os.path.join(self.sandbox, "sdcard", rest)
        if low.startswith(self.SAVE_PREFIX):
            rest = g[len(self.SAVE_PREFIX):].lstrip("/")
            return None, os.path.join(self.sandbox, "data", rest)
        return None, os.path.join(self.sandbox, "other", g.lstrip("/"))

    def resolve_read(self, guest):
        ro, sb = self.map(guest)
        if os.path.exists(sb):
            return sb
        return ro if ro and os.path.exists(ro) else None


def fs_log(e, msg):
    e.fs_events.append(msg)
    e.log(f"[fs] {msg}")


def _open_guest(e, path, writable, truncate, append):
    if writable:
        _, sb = e.fs.map(path)
        os.makedirs(os.path.dirname(sb), exist_ok=True)
        if not truncate and not os.path.exists(sb):
            src = e.fs.resolve_read(path)
            if src and os.path.isfile(src):
                with open(src, "rb") as s, open(sb, "wb") as d:
                    d.write(s.read())
        mode = "ab+" if append else ("wb+" if truncate else ("rb+" if os.path.exists(sb) else "wb+"))
        return open(sb, mode), sb
    host = e.fs.resolve_read(path)
    if host is None or os.path.isdir(host):
        return None, host
    return open(host, "rb"), host


@hle("fopen")
def _fopen(e, n, a):
    path, mode = e.cstr(a.u32()).decode("latin1"), e.cstr(a.u32()).decode("latin1")
    writable = any(c in mode for c in "wa+")
    fh, host = _open_guest(e, path, writable, "w" in mode, "a" in mode)
    if fh is None:
        fs_log(e, f"fopen({path!r}, {mode!r}) -> NULL (not found)")
        e.w32(e.errno_addr, 2)
        return 0
    fp = e.heap_alloc(84)
    e.write(fp + 12, struct.pack("<hh", 0x10 if writable else 0x04, 3 + len(e.files)))
    e.files[fp] = GFile(fh, path, writable)
    fs_log(e, f"fopen({path!r}, {mode!r}) -> {fp:#x} [{os.path.relpath(host, e.root)}]")
    return fp


def _gfile(e, fp, what):
    f = e.files.get(fp)
    if f is None:
        raise ProbeError(f"{what} on unknown FILE* {fp:#x}")
    return f


def _is_std(e, fp):
    base = e.data["__sF"]
    return {base: "stdin", base + 84: "stdout", base + 168: "stderr"}.get(fp)


@hle("fclose")
def _fclose(e, n, a):
    fp = a.u32()
    f = e.files.pop(fp, None)
    if f:
        f.fh.close()
    return 0


@hle("fread")
def _fread(e, n, a):
    ptr, size, cnt, fp = a.u32(), a.u32(), a.u32(), a.u32()
    f = _gfile(e, fp, "fread")
    want = size * cnt
    data = bytes(f.pushback[:want])
    f.pushback = f.pushback[len(data):]
    data += f.fh.read(want - len(data))
    if len(data) < want:
        f.eof = True
    if data:
        e.write(ptr, data)
    return len(data) // size if size else 0


@hle("fwrite")
def _fwrite(e, n, a):
    ptr, size, cnt, fp = a.u32(), a.u32(), a.u32(), a.u32()
    data = e.read(ptr, size * cnt) if size * cnt else b""
    std = _is_std(e, fp)
    if std:
        _emit(e, std, data)
    else:
        f = _gfile(e, fp, "fwrite")
        if not f.writable:
            return 0
        f.fh.write(data)
    return cnt


@hle("fseek")
def _fseek(e, n, a):
    fp, off, whence = a.u32(), a.s32(), a.s32()
    f = _gfile(e, fp, "fseek")
    f.pushback, f.eof = [], False
    try:
        f.fh.seek(off, whence)
        return 0
    except OSError:
        return u32(-1)


@hle("ftell")
def _ftell(e, n, a):
    f = _gfile(e, a.u32(), "ftell")
    return u32(f.fh.tell() - len(f.pushback))


@hle("fgetpos")
def _fgetpos(e, n, a):
    fp, pos = a.u32(), a.u32()
    f = _gfile(e, fp, "fgetpos")
    e.w32(pos, f.fh.tell() - len(f.pushback))
    return 0


@hle("fsetpos")
def _fsetpos(e, n, a):
    fp, pos = a.u32(), a.u32()
    f = _gfile(e, fp, "fsetpos")
    f.pushback, f.eof = [], False
    f.fh.seek(e.r32(pos))
    return 0


@hle("feof")
def _feof(e, n, a): return int(_gfile(e, a.u32(), "feof").eof)


@hle("getc")
def _getc(e, n, a):
    f = _gfile(e, a.u32(), "getc")
    if f.pushback:
        return f.pushback.pop(0)
    b = f.fh.read(1)
    if not b:
        f.eof = True
        return u32(-1)
    return b[0]


@hle("ungetc")
def _ungetc(e, n, a):
    c, fp = a.s32(), a.u32()
    if c < 0:
        return u32(-1)
    f = _gfile(e, fp, "ungetc")
    f.pushback.insert(0, c & 0xFF)
    f.eof = False
    return c & 0xFF


@hle("fflush", "setvbuf")
def _fflush(e, n, a): return 0


@hle("fputs")
def _fputs(e, n, a):
    s, fp = e.cstr(a.u32()), a.u32()
    std = _is_std(e, fp)
    if std:
        _emit(e, std, s)
    else:
        f = _gfile(e, fp, "fputs")
        if f.writable:
            f.fh.write(s)
    return 1


@hle("putc")
def _putc(e, n, a):
    c, fp = a.u32() & 0xFF, a.u32()
    std = _is_std(e, fp)
    if std:
        _emit(e, std, bytes([c]))
    else:
        f = _gfile(e, fp, "putc")
        if f.writable:
            f.fh.write(bytes([c]))
    return c


@hle("fprintf")
def _fprintf(e, n, a):
    fp = a.u32()
    d = cformat(e, e.cstr(a.u32()), a)
    std = _is_std(e, fp)
    if std:
        _emit(e, std, d)
    else:
        f = _gfile(e, fp, "fprintf")
        if f.writable:
            f.fh.write(d)
    return len(d)


# POSIX fds (O_* values are ARM Linux's)
O_WRONLY, O_RDWR, O_CREAT, O_TRUNC, O_APPEND = 1, 2, 0x40, 0x200, 0x400


@hle("open")
def _open(e, n, a):
    path, flags = e.cstr(a.u32()).decode("latin1"), a.u32()
    writable = bool(flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND))
    fh, host = _open_guest(e, path, writable, bool(flags & O_TRUNC), bool(flags & O_APPEND))
    if fh is None:
        fs_log(e, f"open({path!r}, {flags:#x}) -> -1 (not found)")
        e.w32(e.errno_addr, 2)
        return u32(-1)
    fd = e.next_fd
    e.next_fd += 1
    e.fds[fd] = GFile(fh, path, writable)
    fs_log(e, f"open({path!r}, {flags:#x}) -> fd {fd} [{os.path.relpath(host, e.root)}]")
    return fd


@hle("read")
def _read(e, n, a):
    fd, buf, cnt = a.u32(), a.u32(), a.u32()
    f = e.fds.get(fd)
    if f is None:
        e.log(f"WARNING: read on unknown fd {fd}")
        return u32(-1)
    data = f.fh.read(cnt)
    if data:
        e.write(buf, data)
    return len(data)


@hle("write")
def _write(e, n, a):
    fd, buf, cnt = a.u32(), a.u32(), a.u32()
    data = e.read(buf, cnt) if cnt else b""
    if fd in (1, 2):
        _emit(e, "stdout" if fd == 1 else "stderr", data)
        return cnt
    f = e.fds.get(fd)
    if f is None or not f.writable:
        return u32(-1)
    f.fh.write(data)
    return cnt


@hle("lseek")
def _lseek(e, n, a):
    fd, off, whence = a.u32(), a.s32(), a.s32()
    f = e.fds.get(fd)
    if f is None:
        return u32(-1)
    return u32(f.fh.seek(off, whence))


@hle("close")
def _close(e, n, a):
    f = e.fds.pop(a.u32(), None)
    if f:
        f.fh.close()
    return 0


def _write_stat(e, buf, host):
    st = os.stat(host)
    blob = bytearray(104)
    mode = (0o040755 if os.path.isdir(host) else 0o100644)
    struct.pack_into("<I", blob, 16, mode)
    struct.pack_into("<I", blob, 20, 1)
    struct.pack_into("<q", blob, 48, st.st_size)
    struct.pack_into("<I", blob, 56, 4096)
    struct.pack_into("<Q", blob, 64, (st.st_size + 511) // 512)
    for off in (72, 80, 88):
        struct.pack_into("<I", blob, off, int(st.st_mtime) & 0xFFFFFFFF)
    e.write(buf, bytes(blob))


@hle("stat")
def _stat(e, n, a):
    path, buf = e.cstr(a.u32()).decode("latin1"), a.u32()
    host = e.fs.resolve_read(path)
    fs_log(e, f"stat({path!r}) -> {'0' if host else '-1'}")
    if not host:
        e.w32(e.errno_addr, 2)
        return u32(-1)
    _write_stat(e, buf, host)
    return 0


@hle("fstat")
def _fstat(e, n, a):
    fd, buf = a.u32(), a.u32()
    f = e.fds.get(fd)
    if f is None:
        return u32(-1)
    _write_stat(e, buf, f.fh.name)
    return 0


@hle("fcntl")
def _fcntl(e, n, a): return 0


@hle("mkdir")
def _mkdir(e, n, a):
    path = e.cstr(a.u32()).decode("latin1")
    _, sb = e.fs.map(path)
    os.makedirs(sb, exist_ok=True)
    fs_log(e, f"mkdir({path!r}) -> 0 [sandbox]")
    return 0


@hle("remove")
def _remove(e, n, a):
    path = e.cstr(a.u32()).decode("latin1")
    _, sb = e.fs.map(path)
    if os.path.isfile(sb):
        os.remove(sb)
        fs_log(e, f"remove({path!r}) -> 0 [sandbox]")
        return 0
    fs_log(e, f"remove({path!r}) -> -1 (only sandbox files can be removed)")
    return u32(-1)


@hle("chdir")
def _chdir(e, n, a):
    fs_log(e, f"chdir({e.cstr(a.u32())!r}) -> 0 (ignored)")
    return 0


@hle("opendir")
def _opendir(e, n, a):
    path = e.cstr(a.u32()).decode("latin1")
    ro, sb = e.fs.map(path)
    names = set()
    for d in (ro, sb):
        if d and os.path.isdir(d):
            names.update(os.listdir(d))
    if not names and not (ro and os.path.isdir(ro)) and not os.path.isdir(sb):
        fs_log(e, f"opendir({path!r}) -> NULL")
        return 0
    h = e.heap_alloc(4)
    e.dirs[h] = [sorted(names), 0, e.heap_alloc(280), ro, sb]
    fs_log(e, f"opendir({path!r}) -> {h:#x} ({len(names)} entries)")
    return h


@hle("readdir")
def _readdir(e, n, a):
    d = e.dirs.get(a.u32())
    if d is None or d[1] >= len(d[0]):
        return 0
    name = d[0][d[1]]
    d[1] += 1
    full = next((os.path.join(x, name) for x in (d[4], d[3]) if x and os.path.exists(os.path.join(x, name))), "")
    blob = bytearray(280)
    struct.pack_into("<QqHB", blob, 0, d[1], d[1], 280, 4 if os.path.isdir(full) else 8)
    enc = name.encode("utf-8")[:255]
    blob[19:19 + len(enc)] = enc
    e.write(d[2], bytes(blob))
    return d[2]


@hle("closedir")
def _closedir(e, n, a):
    e.dirs.pop(a.u32(), None)
    return 0


# ---------------------------------------------------------------------------
# process, memory, time, misc
@hle("__errno")
def _errno(e, n, a): return e.errno_addr


@hle("sbrk")
def _sbrk(e, n, a):
    incr = a.s32()
    old = e.sbrk(incr)
    if old is None:
        e.log(f"WARNING: sbrk({incr:#x}) failed (heap limit)")
        e.w32(e.errno_addr, 12)
        return u32(-1)
    e.sbrk_calls += 1
    return old


@hle("mmap")
def _mmap(e, n, a):
    addr, length, prot, flags, fd, off = a.u32(), a.u32(), a.u32(), a.u32(), a.s32(), a.u32()
    if flags & 0x20:  # MAP_ANONYMOUS
        p = e.mmap_anon(length)
        e.log(f"[mem] mmap(anon, {length:#x}) -> {p:#x}" if p else f"[mem] mmap(anon, {length:#x}) FAILED")
        return p if p else u32(-1)
    f = e.fds.get(fd)
    if f is None:
        e.log(f"WARNING: mmap of unknown fd {fd}")
        return u32(-1)
    p = e.mmap_anon(length)
    f.fh.seek(off)
    data = f.fh.read(length)
    if p and data:
        e.write(p, data)
    e.log(f"[mem] mmap(fd {fd} {f.path!r}, {length:#x} @ {off:#x}) -> {p:#x}")
    return p if p else u32(-1)


@hle("munmap")
def _munmap(e, n, a):
    e.log(f"[mem] munmap({a.u32():#x}, {a.u32():#x}) (ignored)")
    return 0


@hle("abort")
def _abort(e, n, a):
    raise GuestExit(f"abort() called from {e.where(e.uc.reg_read(emu_lr()))}")


@hle("exit")
def _exit(e, n, a):
    raise GuestExit(f"exit({a.s32()}) called from {e.where(e.uc.reg_read(emu_lr()))}")


@hle("__stack_chk_fail")
def _stack_chk_fail(e, n, a):
    raise ProbeError(f"__stack_chk_fail: stack smashing detected in {e.where(e.uc.reg_read(emu_lr()))}")


@hle("__cxa_pure_virtual")
def _pure_virtual(e, n, a):
    raise ProbeError(f"pure virtual function called from {e.where(e.uc.reg_read(emu_lr()))}")


@hle("__aeabi_atexit")
def _aeabi_atexit(e, n, a):
    obj, fn, dso = a.u32(), a.u32(), a.u32()
    e.atexit.append((fn, obj))
    return 0


@hle("__cxa_guard_acquire")
def _guard_acquire(e, n, a):
    g = a.u32()
    return 0 if e.r32(g) & 1 else 1


@hle("__cxa_guard_release")
def _guard_release(e, n, a):
    e.w32(a.u32(), 1)
    return 0


@hle("_ZnajRKSt9nothrow_t")
def _new_array_nothrow(e, n, a):
    # operator new[](size_t, const std::nothrow_t&) -> the game's own operator new[](size_t)
    return Redirect(e.func("_Znaj"))


@hle("getenv")
def _getenv(e, n, a):
    e.log(f"[env] getenv({e.cstr(a.u32())!r}) -> NULL")
    return 0


@hle("setlocale")
def _setlocale(e, n, a):
    if not hasattr(e, "_c_locale"):
        e._c_locale = e.heap_alloc(2)
        e.write(e._c_locale, b"C\0")
    return e._c_locale


# bionic sysconf names (libc/include/sys/sysconf.h)
_SYSCONF = {0x06: 100, 0x27: 4096, 0x28: 4096, 0x60: 2, 0x61: 2, 0x62: 131072, 0x63: 65536}


@hle("sysconf")
def _sysconf(e, n, a):
    name = a.u32()
    v = _SYSCONF.get(name)
    e.log(f"[env] sysconf({name:#x}) -> {v if v is not None else 'UNKNOWN NAME, -1'}")
    return u32(v if v is not None else -1)


@hle("time")
def _time(e, n, a):
    t = int(time.time())
    p = a.u32()
    if p:
        e.w32(p, t)
    return t


@hle("gettimeofday")
def _gettimeofday(e, n, a):
    tv = a.u32()
    now = time.time()
    if tv:
        e.write(tv, struct.pack("<II", int(now), int((now % 1) * 1e6)))
    return 0


def _tm_blob(t):
    return struct.pack("<9iiI", t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon - 1,
                       t.tm_year - 1900, (t.tm_wday + 1) % 7, t.tm_yday - 1, max(t.tm_isdst, 0), 0, 0)


@hle("localtime", "gmtime")
def _localtime(e, n, a):
    t = s32(e.r32(a.u32()))
    tm = time.localtime(t) if n == "localtime" else time.gmtime(t)
    if not hasattr(e, "_tm_buf"):
        e._tm_buf = e.heap_alloc(44)
    e.write(e._tm_buf, _tm_blob(tm))
    return e._tm_buf


@hle("mktime")
def _mktime(e, n, a):
    v = struct.unpack("<9i", e.read(a.u32(), 36))
    return u32(int(time.mktime((v[5] + 1900, v[4] + 1, v[3], v[2], v[1], v[0], 0, 0, -1))))


@hle("sleep", "usleep", "sched_yield")
def _sleep(e, n, a): return 0


@hle("lrand48")
def _lrand48(e, n, a):
    e.rand48 = (0x5DEECE66D * e.rand48 + 0xB) & ((1 << 48) - 1)
    return e.rand48 >> 17


@hle("srand48")
def _srand48(e, n, a):
    e.rand48 = ((a.u32() << 16) | 0x330E) & ((1 << 48) - 1)
    return 0


@hle("qsort")
def _qsort(e, n, a):
    base, cnt, size, cmp = a.u32(), a.u32(), a.u32(), a.u32()
    if cnt < 2 or size == 0:
        return 0
    order = sorted(range(cnt), key=functools.cmp_to_key(
        lambda i, j: s32(e.call(cmp, base + i * size, base + j * size)[0])))
    blob = e.read(base, cnt * size)
    e.write(base, b"".join(blob[i * size:(i + 1) * size] for i in order))
    return 0


@hle("setjmp")
def _setjmp(e, n, a):
    from unicorn import arm_const as A
    buf = a.u32()
    regs = [e.uc.reg_read(getattr(A, f"UC_ARM_REG_R{i}")) for i in range(4, 12)]
    e.jmpbufs[buf] = (regs, e.uc.reg_read(A.UC_ARM_REG_SP), e.uc.reg_read(A.UC_ARM_REG_LR))
    e.w32(buf, 0x4A4D5042)  # 'BPMJ' marker
    return 0


@hle("longjmp")
def _longjmp(e, n, a):
    from unicorn import arm_const as A
    buf, val = a.u32(), a.u32()
    if buf not in e.jmpbufs:
        raise ProbeError(f"longjmp to unknown jmp_buf {buf:#x}")
    regs, sp, lr = e.jmpbufs[buf]
    for i, v in zip(range(4, 12), regs):
        e.uc.reg_write(getattr(A, f"UC_ARM_REG_R{i}"), v)
    e.uc.reg_write(A.UC_ARM_REG_SP, sp)
    e.uc.reg_write(A.UC_ARM_REG_R0, val or 1)
    return Redirect(lr)


# -- pthreads (single-threaded probe) --------------------------------------
@hle("pthread_mutex_init", "pthread_mutex_lock", "pthread_mutex_unlock", "pthread_mutex_trylock",
     "pthread_mutex_destroy", "pthread_mutexattr_init", "pthread_mutexattr_settype",
     "pthread_mutexattr_destroy", "pthread_cond_init", "pthread_cond_destroy",
     "pthread_cond_signal", "pthread_cond_broadcast", "pthread_attr_init", "pthread_attr_destroy",
     "pthread_attr_setdetachstate", "pthread_attr_setstacksize", "pthread_setschedparam",
     "pthread_getschedparam", "sched_get_priority_max", "sched_get_priority_min")
def _pthread_ok(e, n, a): return 0


@hle("pthread_cond_wait")
def _cond_wait(e, n, a):
    e.log(f"WARNING: pthread_cond_wait in single-threaded probe (returning immediately) [from {e.where(e.uc.reg_read(emu_lr()))}]")
    return 0


@hle("pthread_cond_timedwait")
def _cond_timedwait(e, n, a): return 110  # ETIMEDOUT


@hle("pthread_self")
def _pthread_self(e, n, a): return 0x1000


@hle("pthread_equal")
def _pthread_equal(e, n, a): return int(a.u32() == a.u32())


@hle("pthread_key_create")
def _key_create(e, n, a):
    kp = a.u32()
    e.w32(kp, len(e.tls))
    e.tls.append(0)
    return 0


@hle("pthread_getspecific")
def _getspecific(e, n, a):
    k = a.u32()
    return e.tls[k] if k < len(e.tls) else 0


@hle("pthread_setspecific")
def _setspecific(e, n, a):
    k, v = a.u32(), a.u32()
    if k < len(e.tls):
        e.tls[k] = v
    return 0


@hle("pthread_create")
def _pthread_create(e, n, a):
    th, attr, start, arg = a.u32(), a.u32(), a.u32(), a.u32()
    tid = 0x2000 + len(e.threads)
    e.threads.append((start, arg))
    if th:
        e.w32(th, tid)
    e.log(f"STUB: pthread_create(start={e.where(start)}, arg={arg:#x}) -> thread NOT started (probe is single-threaded)")
    return 0


@hle("pthread_join")
def _pthread_join(e, n, a): return 0


# ---------------------------------------------------------------------------
def install(emu, rep, data_dir=None, root=None):
    root = root or os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    emu.root = root
    emu.handlers.update(H)
    emu.default_handler = default_stub
    emu.stubbed = set()
    emu.files, emu.fds, emu.dirs, emu.jmpbufs = {}, {}, {}, {}
    emu.next_fd = 3
    emu.fs_events = []
    emu.atexit, emu.tls, emu.threads = [], [], []
    emu.rand48 = 0x1234ABCD330E
    emu.sbrk_calls = 0
    emu.errno_addr = emu.data_alloc(4, 4)
    sandbox = os.path.join(root, "work", "probe_sandbox")
    os.makedirs(sandbox, exist_ok=True)
    emu.fs = FS(data_dir or os.path.join(root, "original", "gameloft", "games", "Gangstar2"), sandbox)
