#!/usr/bin/env python3
"""Find the first instruction where the native build leaves the oracle.

Both builds drive the same host and therefore the same SWIV_STATELOG
machinery, which records pc + D0-D7 + A0-A7 + SR before every
instruction.  So the native build can be judged not by whether it
eventually looks wrong, but by the exact instruction at which it stops
agreeing -- which is the difference between debugging and guessing.
"""
import struct, sys, argparse, subprocess, re

REC = struct.Struct('<18I')
NAMES = ['pc'] + ['d%d' % i for i in range(8)] + ['a%d' % i for i in range(8)] + ['sr']

def load(p):
    d = open(p, 'rb').read()
    return [REC.unpack_from(d, i * 72) for i in range(len(d) // 72)]

ap = argparse.ArgumentParser()
ap.add_argument('oracle')
ap.add_argument('native')
ap.add_argument('--image', default='re/pipeline/decode.bin')
ap.add_argument('--context', type=int, default=6)
ap.add_argument('--ignore-sr', action='store_true', default=True)
ap.add_argument('--pc-only', action='store_true',
                help='compare control flow only.  Register values legitimately '
                     'differ wherever the game reads the beam position, so a '
                     'full-register lockstep reports noise; the pc stream is '
                     'the thing that must not diverge.')
a = ap.parse_args()

o, n = load(a.oracle), load(a.native)
print("oracle %d records, native %d records" % (len(o), len(n)))
lim = min(len(o), len(n))
first = None
for i in range(lim):
    ro, rn = o[i], n[i]
    fields = (0,) if a.pc_only else range(0, 17)
    if any(ro[k] != rn[k] for k in fields):
        first = i
        break
if first is None:
    print("no divergence in the first %d instructions" % lim)
    sys.exit(0)

lo = max(0, first - a.context)
text = subprocess.run(['./build/dasm', a.image,
                       hex(min(r[0] for r in o[lo:first + 2])),
                       hex(max(r[0] for r in o[lo:first + 2]) + 8)],
                      capture_output=True, text=True).stdout
dis = {}
for line in text.splitlines():
    m = re.match(r'\$([0-9a-f]{6})\s+(.*)', line)
    if m: dis[int(m.group(1), 16)] = m.group(2).rstrip()

print("\nfirst divergence at instruction %d\n" % first)
for i in range(lo, min(first + 2, lim)):
    mark = '>>' if i == first else '  '
    ro, rn = o[i], n[i]
    diff = ' '.join('%s: oracle $%08x native $%08x' % (NAMES[k], ro[k], rn[k])
                    for k in range(0, 17) if ro[k] != rn[k])
    print("%s $%06x  %-34s %s" % (mark, ro[0], dis.get(ro[0], ''), diff))
