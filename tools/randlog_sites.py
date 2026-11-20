#!/usr/bin/env python3
"""Compare RNG call-site histograms from two harness runs.

Usage:
    python randlog-sites.py runA-rand.txt runB-rand.txt --map MatrixGame.map --dll MatrixGame.dll

The harness logs, for every logged tick, how many rand() calls came from each
return address (a module offset). When two runs diverge in the number of RNG
calls, this points at the function whose control flow depended on the differing
value -- usually a branch on a field that was never initialized.

Symbols come from the linker .map file (build with -DMATRIXGAME_SIM_HARNESS=ON,
which enables /MAP). Anonymous-namespace helpers are not listed there, so an
unknown offset is reported as the nearest preceding named symbol + delta.
"""

import argparse
import struct
import sys


def image_base(path):
    with open(path, "rb") as f:
        data = f.read(0x400)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("not a PE file: %s" % path)
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x10B:
        raise SystemExit("not a PE32 image: %s" % path)
    return struct.unpack_from("<I", data, opt + 28)[0]


def read_map(path, base):
    """Returns sorted [(offset, name)] of named functions."""
    syms = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            # 0001:000b3ec0  ?SimRand@@YIIXZ  100b4ec0  f  SimHarness.obj
            if len(parts) < 4 or ":" not in parts[0] or not parts[2].startswith(("100", "0")):
                continue
            if not all(c in "0123456789abcdefABCDEF" for c in parts[2]):
                continue
            try:
                addr = int(parts[2], 16)
            except ValueError:
                continue
            syms.append((addr - base, parts[1]))
    syms.sort()
    return syms


def nearest(syms, off):
    lo, hi = 0, len(syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= off:
            best = syms[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    if best is None:
        return "0x%x" % off
    return "%s+0x%x" % (undemangle(best[1]), off - best[0])


def undemangle(name):
    """Trims an MSVC decorated name down to something readable."""
    if not name.startswith("?"):
        return name
    name = name[1:]
    cut = min([i for i in (name.find("@"),) if i != -1] or [len(name)])
    return name[:cut]


def read_randlog(path):
    ticks = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("t="):
                continue
            fields = line.split()
            t = int(fields[0][2:])
            total = None
            sites = {}
            for tok in fields[1:]:
                k, _, v = tok.partition("=")
                if k == "total":
                    total = int(v)
                elif k.startswith("s"):
                    sites[int(k[1:], 16)] = int(v)
            ticks[t] = (total, sites)
    return ticks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--map", default="build-sim/MatrixGame/Release/MatrixGame.map")
    ap.add_argument("--dll", default="build-sim/MatrixGame/Release/MatrixGame.dll")
    args = ap.parse_args()

    base = image_base(args.dll)
    syms = read_map(args.map, base)
    a = read_randlog(args.a)
    b = read_randlog(args.b)

    common = sorted(set(a) & set(b))
    print("A: %s (%d ticks)" % (args.a, len(a)))
    print("B: %s (%d ticks)" % (args.b, len(b)))
    print("map: %d named symbols, image base 0x%08x" % (len(syms), base))

    first = None
    for t in common:
        ta, sa = a[t]
        tb, sb = b[t]
        if ta != tb or sa != sb:
            first = t
            break

    if first is None:
        print("\nIDENTICAL: same rand() totals and same per-site deltas for %d ticks" % len(common))
        return 0

    ta, sa = a[first]
    tb, sb = b[first]
    print("\nFIRST DIVERGENCE at t=%d: total %s vs %s" % (first, ta, tb))
    rows = []
    for site in sorted(set(sa) | set(sb)):
        da, db = sa.get(site, 0), sb.get(site, 0)
        if da != db:
            rows.append((site, da, db))
    if not rows:
        print("  totals differ but per-site deltas are equal (aggregation artifact)")
        return 1

    rows.sort(key=lambda r: -abs(r[1] - r[2]))
    print("  %-46s %8s %8s %8s" % ("site", "A", "B", "delta"))
    for site, da, db in rows:
        print("  %-46s %8d %8d %+8d" % (nearest(syms, site) + "  [0x%x]" % site, da, db, db - da))

    # Also show the nearest matching tick before the divergence for context.
    prev = [t for t in common if t < first]
    if prev:
        p = prev[-1]
        print("\n  at t=%d totals were %s vs %s (equal)" % (p, a[p][0], b[p][0]))
    return 1


if __name__ == "__main__":
    sys.exit(main())
