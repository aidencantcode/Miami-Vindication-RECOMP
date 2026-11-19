"""Compare two guest memory dumps (GANGSTAR_MEMDUMP_AT, written next to the import trace).

Usage: python -I tools/recomp/mem_diff.py <unicorn .mem> <recomp .mem> [--max N] [--skip-stack]

Prints, per region, the differing 4-byte words (address, unicorn value, recomp value), grouped
into runs. Taken at the same import call with GANGSTAR_FIXED_TIME, the two backends should agree
everywhere except the live stack below sp (return-to-host sentinels differ).
"""
import struct
import sys


def load(path):
    regions = {}
    with open(path, "rb") as f:
        data = f.read()
    i = 0
    while i + 12 <= len(data):
        addr, size, nlen = struct.unpack_from("<III", data, i)
        i += 12
        name = data[i:i + nlen].decode("ascii", "replace")
        i += nlen
        regions[(addr, name)] = data[i:i + size]
        i += size
    return regions


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    maxn = int(argv[argv.index("--max") + 1]) if "--max" in argv else 40
    skip_stack = "--skip-stack" in argv
    a, b = load(argv[0]), load(argv[1])
    total = 0
    for key in sorted(a):
        addr, name = key
        if key not in b:
            print(f"region {name} @{addr:08x}: only in the first dump")
            continue
        if skip_stack and "stack" in name:
            continue
        x, y = a[key], b[key]
        if x == y:
            continue
        n = min(len(x), len(y))
        diffs = [o for o in range(0, n - 3, 4) if x[o:o + 4] != y[o:o + 4]]
        print(f"region {name} @{addr:08x} size {len(x):#x}: {len(diffs)} differing words")
        runs = []
        for o in diffs:
            if runs and o - runs[-1][1] <= 16:
                runs[-1][1] = o
            else:
                runs.append([o, o])
        for lo, hi in runs[:maxn]:
            words = range(lo, hi + 4, 4)
            print(f"  {addr + lo:08x}..{addr + hi + 3:08x}")
            for o in list(words)[:8]:
                u, r = struct.unpack_from("<I", x, o)[0], struct.unpack_from("<I", y, o)[0]
                mark = "  " if u == r else "!="
                print(f"    {addr + o:08x}  U {u:08x}  R {r:08x} {mark}")
        if len(runs) > maxn:
            print(f"  ... {len(runs) - maxn} more runs")
        total += len(diffs)
    print(f"{total} differing words in total")
    return 0 if total == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
