#!/usr/bin/env python3
"""Compare two directories of PPM frames and demand they be identical.

The snapshot gates prove individual routines; this proves the whole
machine.  Anything short of 100% is a real difference, not rounding:
these are the same bytes out of the same rasteriser, so either the game
did the same thing or it did not.
"""
import glob, os, sys

def load(p):
    d = open(p, 'rb').read()
    i = 0
    for _ in range(3):
        i = d.index(b'\n', i) + 1
    return d[i:]

o_dir, n_dir = sys.argv[1], sys.argv[2]
ok = tot = 0
bad = []
for f in sorted(glob.glob(os.path.join(o_dir, 'f*.ppm'))):
    n = os.path.join(n_dir, os.path.basename(f))
    tot += 1
    if not os.path.exists(n):
        bad.append((os.path.basename(f), -1.0))
        continue
    a, b = load(f), load(n)
    same = sum(1 for x, y in zip(a, b) if x == y)
    pct = 100.0 * same / len(a)
    if pct == 100.0:
        ok += 1
    else:
        bad.append((os.path.basename(f), pct))
print("frame gate: %d / %d checkpoints pixel-identical" % (ok, tot))
for name, pct in bad:
    print("   %s %s" % (name, "MISSING" if pct < 0 else "%7.3f%%" % pct))
sys.exit(1 if bad else 0)
