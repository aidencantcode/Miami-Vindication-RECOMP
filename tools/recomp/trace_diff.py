"""Find the first import call where two traces differ (GANGSTAR_TRACE_IMPORTS output).

Usage: python -I tools/recomp/trace_diff.py <unicorn trace> <recomp trace> [--context N] [--loose]

Both CPU backends write one line per import call ("<n> <name> lr=<addr> r0 r1 r2 r3"). Calls that
only one backend makes (the soft-float helpers the recompiler emits inline) are dropped before
comparing. --loose compares only the name and return address, not the argument registers.
"""
import sys


def load(path):
    out = []
    with open(path, "r", encoding="ascii", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 7:
                out.append((parts[1], parts[2], tuple(parts[3:7]), parts[0]))
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    context, loose = 12, "--loose" in argv
    if "--context" in argv:
        context = int(argv[argv.index("--context") + 1])
    a, b = load(argv[0]), load(argv[1])
    common = {x[0] for x in a} & {x[0] for x in b}
    only = sorted(({x[0] for x in a} | {x[0] for x in b}) - common)
    if only:
        print(f"ignored (called by one backend only): {', '.join(only)}")
    a = [x for x in a if x[0] in common]
    b = [x for x in b if x[0] in common]
    n = min(len(a), len(b))
    for i in range(n):
        same = a[i][:2] == b[i][:2] and (loose or a[i][2] == b[i][2])
        if not same:
            print(f"first difference at filtered call {i} (unicorn line {a[i][3]}, recomp line {b[i][3]}):")
            for j in range(max(0, i - context), min(n, i + 4)):
                mark = ">>" if j == i else "  "
                print(f"{mark} U {a[j][0]:<28} {a[j][1]} {' '.join(a[j][2])}")
                if a[j] [:3] != b[j][:3]:
                    print(f"{mark} R {b[j][0]:<28} {b[j][1]} {' '.join(b[j][2])}")
            return 1
    print(f"no difference in the first {n} calls ({len(a)} unicorn, {len(b)} recomp after filtering)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
