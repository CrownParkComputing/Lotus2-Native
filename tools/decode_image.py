#!/usr/bin/env python3
"""Build a decode image in which EVERY region is coherent with the moment
its code actually ran.

combined.bin is one snapshot of memory.  Chip RAM is reused: code loaded
from disk, executed, then overwritten by data.  Disassembling such a
region from the wrong moment yields plausible-looking nonsense -- 68020
addressing modes and chk.w (A6)+ are what data looks like through a
disassembler.

So the decode image is assembled per region: for each range given, the
bytes are taken from a chip snapshot captured at the pc where that code
runs, rather than from the single global dump.

  python3 tools/decode_image.py --base re/pipeline/combined.bin \\
      --region 723b0-72690=re/pipeline/boot/chip_0723ba.bin \\
      --out re/pipeline/decode.bin

A region must cover the full extent of its last instruction, not just the
last address the pc reached: a `jmp $xxxxxxxx.l` at the final executed pc
reads four operand bytes past it, and taking those from the stale image
produces a wild jump that looks like memory corruption.  Pad the end.
"""
import argparse

ap = argparse.ArgumentParser()
ap.add_argument('--base', default='re/pipeline/combined.bin')
ap.add_argument('--region', action='append', default=[],
                help='LO-HI=CHIP_SNAPSHOT')
ap.add_argument('--out', default='re/pipeline/decode.bin')
a = ap.parse_args()

img = bytearray(open(a.base, 'rb').read())
for spec in a.region:
    rng, path = spec.split('=')
    lo, hi = (int(x, 16) for x in rng.split('-'))
    src = open(path, 'rb').read()
    # a chip snapshot is indexed from $000000, an ExpMem one from $200000
    base = 0 if lo < 0x80000 else 0x200000
    if hi - base > len(src):
        raise SystemExit('%s is too small for $%06x-$%06x' % (path, lo, hi))
    changed = sum(1 for i in range(lo, hi) if img[i] != src[i - base])
    img[lo:hi] = src[lo - base:hi - base]
    print('$%06x-$%06x from %s (%d of %d bytes differed)'
          % (lo, hi, path, changed, hi - lo))
open(a.out, 'wb').write(img)
print('wrote %s (%d bytes)' % (a.out, len(img)))
