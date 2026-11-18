"""Bionic (Android 2.x, 32-bit ARM) data objects the game imports directly.
Layouts follow bionic's OpenBSD-derived headers of that era:

- `__sF`: `FILE __sF[3]` (stdin, stdout, stderr); sizeof(FILE) = 84.
    offsets: _p 0, _r 4, _w 8, _flags 12 (short), _file 14 (short), ...
- `_ctype_`: `const char *_ctype_`, a POINTER to a 1+256 byte table
  (entry 0 is EOF). Usage: `(_ctype_ + 1)[c] & _N`.
- `_tolower_tab_` / `_toupper_tab_`: `const short *`, POINTERS to 1+256 shorts.
- `__stack_chk_guard`: a 4-byte canary value.
- `__dso_handle`, `std::nothrow`: only their addresses matter.

The pointer-vs-table assumption for `_ctype_`/`_to*_tab_` is checked at
runtime: the probe watches reads of these objects (see emu.watch()).
"""
import struct

FILE_SIZE = 84

# OpenBSD ctype bits as used by bionic
_U, _L, _N, _S, _P, _C, _X, _B = 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80


def ctype_table():
    t = bytearray(257)  # [0] = EOF
    for c in range(256):
        f = 0
        if c < 0x20 or c == 0x7F:
            f |= _C
        if 0x09 <= c <= 0x0D:
            f |= _S
        if c == 0x20:
            f |= _S | _B
        if 0x21 <= c <= 0x7E and not chr(c).isalnum():
            f |= _P
        if 0x30 <= c <= 0x39:
            f |= _N
        if 0x41 <= c <= 0x5A:
            f |= _U
        if 0x61 <= c <= 0x7A:
            f |= _L
        if 0x41 <= c <= 0x46 or 0x61 <= c <= 0x66:
            f |= _X
        t[1 + c] = f
    return bytes(t)


def case_table(upper):
    vals = [-1]
    for c in range(256):
        if upper and 0x61 <= c <= 0x7A:
            vals.append(c - 0x20)
        elif not upper and 0x41 <= c <= 0x5A:
            vals.append(c + 0x20)
        else:
            vals.append(c)
    return struct.pack("<257h", *vals)


def build(alloc, write):
    """Create all objects. alloc(size, align) -> addr; write(addr, bytes).
    Returns {symbol name: address of the object}, plus helper entries."""
    out = {}
    # __sF: stdin/stdout/stderr
    sf = alloc(3 * FILE_SIZE, 8)
    blob = bytearray(3 * FILE_SIZE)
    for i, flags in enumerate((0x0004, 0x0008, 0x0008 | 0x0002)):  # __SRD, __SWR, __SWR|__SNBF
        struct.pack_into("<hh", blob, i * FILE_SIZE + 12, flags, i)
    write(sf, bytes(blob))
    out["__sF"] = sf

    for name, table in (("_ctype_", ctype_table()),
                        ("_tolower_tab_", case_table(False)),
                        ("_toupper_tab_", case_table(True))):
        tab = alloc(len(table), 8)
        write(tab, table)
        ptr = alloc(4, 4)
        write(ptr, struct.pack("<I", tab))
        out[name] = ptr
        out[name + "#table"] = tab

    guard = alloc(4, 4)
    write(guard, struct.pack("<I", 0xC0DEC0DE))
    out["__stack_chk_guard"] = guard
    out["__dso_handle"] = alloc(4, 4)
    out["_ZSt7nothrow"] = alloc(4, 4)
    return out


DATA_IMPORTS = {"__sF", "_ctype_", "_tolower_tab_", "_toupper_tab_",
                "__stack_chk_guard", "__dso_handle", "_ZSt7nothrow"}
