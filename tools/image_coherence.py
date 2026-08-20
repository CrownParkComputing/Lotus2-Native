#!/usr/bin/env python3
"""Check that the decode image matches memory AT THE MOMENT each region runs.

The decode image is one snapshot of a machine whose memory is reused.
Code is loaded from disk, executed, and later overwritten by data -- in
chip RAM and, as it turns out, in ExpMem too.  Disassembling a region
from the wrong moment produces something that compiles and is wrong: a
buffer of zeros reads back as a run of `ori.b #$0,D0`, so the CPU walks
through it instead of faulting, and the failure surfaces thousands of
frames later somewhere unrelated.

For each contiguous region of executed pcs this snapshots memory at that
region's FIRST pc and compares. Anything under 100% needs a per-region
overlay in the decode image; the check is what turns that from
whack-a-mole into a list.
"""
import subprocess, sys, os, glob, argparse

ap = argparse.ArgumentParser()
ap.add_argument('--pcset', default='re/pipeline/pcset_race.txt')
ap.add_argument('--image', default='re/pipeline/decode.bin')
ap.add_argument('--install', default='original/Lotus2CD32')
ap.add_argument('--tmp', default='re/pipeline/boot')
a = ap.parse_args()

pcs = sorted(int(l.strip(), 16) for l in open(a.pcset) if l.strip())
regions, prev = [], None
for p in pcs:
    if prev is None or p - prev > 0x400:
        regions.append([p, p + 2])
    else:
        regions[-1][1] = p + 2
    prev = p

img = open(a.image, 'rb').read()
os.makedirs(a.tmp, exist_ok=True)
bad = []
print("%-24s %8s  %s" % ("region", "match", "verdict"))
for lo, hi in regions:
    pre = os.path.join(a.tmp, 'coh_')
    for f in glob.glob(pre + '*'):
        os.remove(f)
    env = dict(os.environ, SWIV_SNAP_PCS='%x' % lo, SWIV_SNAP_FROM='0',
               SWIV_SNAP_MAX='1', SWIV_SNAP_PREFIX=pre)
    subprocess.run(['./build/lotus2', '--dir', a.install, '--frames', '9000',
                    '--fire-from', '2100', '--fire-period', '100'],
                   env=env, capture_output=True)
    chip = glob.glob(pre + '*_chip.bin')
    fast = glob.glob(pre + '*_fast.bin')
    if not chip or not fast:
        print("$%06x-$%06x %8s  never reached in this run" % (lo, hi, '-'))
        continue
    live = open(chip[0], 'rb').read() if lo < 0x80000 else open(fast[0], 'rb').read()
    base = 0 if lo < 0x80000 else 0x200000
    same = sum(1 for i in range(lo, hi) if img[i] == live[i - base])
    pct = 100.0 * same / (hi - lo)
    ok = pct == 100.0
    if not ok:
        bad.append((lo, hi, pct))
    print("$%06x-$%06x %7.1f%%  %s" % (lo, hi, pct,
          "coherent" if ok else "STALE -- needs a per-region overlay"))
if bad:
    print("\n%d stale region(s):" % len(bad))
    for lo, hi, pct in bad:
        print("  --region %x-%x   (%.1f%% match)" % (lo, hi + 0x20, pct))
sys.exit(1 if bad else 0)
