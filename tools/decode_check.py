#!/usr/bin/env python3
"""Verify that our disassembly's instruction boundaries agree with the
PCs the CPU actually executed.

Static recompilation stands or falls on the decode.  If a region is
decoded one byte out of phase -- data mistaken for code, a jump table
walked as instructions -- the output still compiles and still looks
plausible.  The oracle already knows every address the 68000 fetched
from, so the check is free: EVERY executed PC must be the start of an
instruction in our decode, and any that is not means that region is
mis-decoded and must not be translated.
"""
import subprocess, re, sys, argparse

def boundaries(image, lo, hi):
    out = subprocess.run(['./build/dasm', image, hex(lo), hex(hi)],
                         capture_output=True, text=True).stdout
    return set(int(m.group(1), 16)
               for m in re.finditer(r'^\$([0-9a-f]{6})', out, re.M))

def regions(pcs, gap=0x400):
    r, start, prev = [], pcs[0], pcs[0]
    for p in pcs[1:]:
        if p - prev > gap:
            r.append((start, prev)); start = p
        prev = p
    r.append((start, prev))
    return r

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', default='re/pipeline/combined.bin')
    ap.add_argument('--pcset', default='re/pipeline/pcset_race.txt')
    a = ap.parse_args()
    pcs = sorted(int(l.strip(), 16) for l in open(a.pcset) if l.strip())
    bad_total = 0
    print("%-22s %7s %7s  %s" % ("region", "PCs", "off-bnd", "verdict"))
    for lo, hi in regions(pcs):
        b = boundaries(a.image, lo, hi + 8)
        here = [p for p in pcs if lo <= p <= hi]
        bad = [p for p in here if p not in b]
        bad_total += len(bad)
        verdict = "OK" if not bad else "MIS-DECODED"
        print("$%06x-$%06x %7d %7d  %s%s"
              % (lo, hi, len(here), len(bad), verdict,
                 '' if not bad else '  first: ' +
                 ' '.join('$%06x' % p for p in bad[:4])))
    print("\n%d executed PCs off an instruction boundary" % bad_total)
    return 1 if bad_total else 0

sys.exit(main())
