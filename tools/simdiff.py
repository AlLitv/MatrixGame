#!/usr/bin/env python3
"""Compare two deterministic-harness logs.

Usage:
    python simdiff.py runA.txt runB.txt             # hash logs (t=... hash=...)
    python simdiff.py runA-obj.txt runB-obj.txt     # per-object dumps

Hash logs are compared tick by tick: the first tick with a different hash is
reported together with the object count and RNG call count on both sides.

Object dumps are keyed by the stable object id, so a difference is reported as
"object N, field F: A vs B" instead of "the world diverged somewhere". That is
what points at the exact field that was read before being initialized.
"""

import sys
from collections import Counter, defaultdict

SKIP = {"t", "id"}


def read_hashlog(path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("t="):
                continue
            fields = dict(p.split("=", 1) for p in line.split() if "=" in p)
            fields["final"] = line.startswith("FINAL")
            rows.append(fields)
    return rows


def cmp_hashlog(path_a, path_b):
    a = read_hashlog(path_a)
    b = read_hashlog(path_b)

    print("A: %s (%d lines)" % (path_a, len(a)))
    print("B: %s (%d lines)" % (path_b, len(b)))

    n = min(len(a), len(b))
    for i in range(n):
        if a[i].get("hash") != b[i].get("hash"):
            print("\nFIRST DIVERGENCE at line %d" % (i + 1))
            for side, row in (("A", a[i]), ("B", b[i])):
                print("  %s: t=%s objects=%s rand=%s hash=%s" %
                      (side, row.get("t"), row.get("objects"), row.get("rand"), row.get("hash")))
            prev = a[i - 1] if i else None
            if prev:
                print("  last matching tick: t=%s hash=%s" % (prev.get("t"), prev.get("hash")))
            return 1

    if len(a) != len(b):
        print("\nSame hashes for the first %d ticks, but line counts differ "
              "(A=%d, B=%d) - one run stopped earlier." % (n, len(a), len(b)))
        return 1

    last = a[-1] if a else {}
    print("\nIDENTICAL: %d ticks, last t=%s objects=%s rand=%s hash=%s" %
          (len(a), last.get("t"), last.get("objects"), last.get("rand"), last.get("hash")))
    return 0


def read_objlog(path):
    frames = defaultdict(dict)
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("t="):
                continue
            fields = dict(p.split("=", 1) for p in line.split() if "=" in p)
            try:
                t = int(fields["t"])
                oid = int(fields["id"])
            except (KeyError, ValueError):
                continue
            frames[t][oid] = fields
    return frames


def cmp_objlog(path_a, path_b, max_fields=40):
    a = read_objlog(path_a)
    b = read_objlog(path_b)

    print("A: %s (%d ticks)" % (path_a, len(a)))
    print("B: %s (%d ticks)" % (path_b, len(b)))

    ticks = sorted(set(a) & set(b))
    if not ticks:
        print("\nNo common ticks - nothing to compare.")
        return 1

    diff_ticks = 0
    first = None
    field_counter = Counter()
    object_counter = Counter()
    shifted_ticks = 0

    for t in ticks:
        fa, fb = a[t], b[t]
        # The id counter is global, so anything created before the battle (the
        # map preview in the game menu) shifts every id by a constant. When the
        # id sets differ but the counts match, objects are the same ones in the
        # same creation order, so compare them positionally.
        if set(fa) != set(fb) and len(fa) == len(fb):
            shifted_ticks += 1
            pairs_ids = list(zip(sorted(fa), sorted(fb)))
        elif set(fa) != set(fb):
            pairs_ids = [(oid, oid) for oid in sorted(set(fa) | set(fb))]
        else:
            pairs_ids = [(oid, oid) for oid in sorted(fa)]

        pairs = []
        for oid_a, oid_b in pairs_ids:
            ra, rb = fa.get(oid_a), fb.get(oid_b)
            if ra is None or rb is None:
                pairs.append((oid_a, "present", "yes" if ra else "no", "yes" if rb else "no"))
                continue
            for k in sorted(set(ra) | set(rb)):
                if k in SKIP:
                    continue
                va, vb = ra.get(k), rb.get(k)
                if va != vb:
                    pairs.append((oid_a, k, va, vb))
        if pairs:
            diff_ticks += 1
            if first is None:
                first = (t, pairs)
            for oid, k, va, vb in pairs:
                field_counter[k] += 1
                object_counter[oid] += 1

    if not diff_ticks:
        print("\nIDENTICAL: %d ticks compared, every object field matches." % len(ticks))
        return 0

    t, pairs = first
    print("\nFIRST DIVERGENCE at t=%d: %d differing fields across %d objects" %
          (t, len(pairs), len({p[0] for p in pairs})))
    for oid, k, va, vb in pairs[:max_fields]:
        print("  id=%-5d %-8s A=%-12s B=%s" % (oid, k, va, vb))
    if len(pairs) > max_fields:
        print("  ... %d more" % (len(pairs) - max_fields))

    print("\nDiverging ticks: %d of %d (%.1f%%)" % (diff_ticks, len(ticks), 100.0 * diff_ticks / len(ticks)))
    if shifted_ticks:
        print("Id sets differ on %d ticks (global id counter shifted by the menu preview); "
              "those ticks were aligned by creation order." % shifted_ticks)
    print("Top fields:")
    for k, c in field_counter.most_common(15):
        print("  %-8s %d" % (k, c))
    print("Top objects:")
    for oid, c in object_counter.most_common(10):
        print("  id=%-5d %d" % (oid, c))
    return 1


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    path_a, path_b = argv[1], argv[2]
    try:
        with open(path_a, "r", encoding="utf-8") as f:
            head = f.readline()
    except OSError as exc:
        print("Cannot read %s: %s" % (path_a, exc))
        return 2
    if "id=" in head:
        return cmp_objlog(path_a, path_b)
    return cmp_hashlog(path_a, path_b)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
