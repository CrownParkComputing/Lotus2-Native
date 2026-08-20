#!/usr/bin/env python3
"""Extract ONE call of a 68k routine from a statelog, annotated.

The host's SWIV_STATELOG writes 18 longs per instruction: PC, D0-D7,
A0-A7, SR -- sampled BEFORE the instruction runs.  So the difference
between consecutive records is exactly what the instruction did, and a
routine's whole execution can be replayed with real numbers instead of
inferred ones.

That is the difference between porting from a disassembly and porting
from evidence.  "move.w ($30d8,A3),D1" does not tell you whether the
game wanted the high or the low word of that long; the trace shows D1
becoming $0007 and settles it in one line.

Usage:
  python3 tools/trace_call.py STATELOG ENTRY_PC [--image combined.bin]
                              [--nth N] [--max N] [--all-regs]

Prints: address, disassembly, and the registers the instruction changed.
"""
import struct, subprocess, sys, re, argparse

REC = struct.Struct('>18I')   # host writes native-endian; fixed up below
REC_LE = struct.Struct('<18I')

def load(path):
    data = open(path, 'rb').read()
    n = len(data) // 72
    # the host fwrite()s native uint32; detect by looking at the first PC
    for st in (REC_LE, REC):
        pc = st.unpack_from(data, 0)[0]
        if 0 < pc < 0x400000:
            return data, n, st
    raise SystemExit('cannot determine statelog endianness')

def disasm(image, lo, hi):
    out = subprocess.run(['./build/dasm', image, hex(lo), hex(hi)],
                         capture_output=True, text=True).stdout
    m = {}
    for line in out.splitlines():
        mm = re.match(r'\$([0-9a-f]{6})\s+(.*)', line)
        if mm:
            m[int(mm.group(1), 16)] = mm.group(2).rstrip()
    return m

NAMES = ['pc'] + ['d%d' % i for i in range(8)] + ['a%d' % i for i in range(8)] + ['sr']

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('statelog')
    ap.add_argument('entry', type=lambda s: int(s, 16))
    ap.add_argument('--image', default='re/pipeline/combined.bin')
    ap.add_argument('--nth', type=int, default=0,
                    help='which call of the routine (0 = first)')
    ap.add_argument('--max', type=int, default=4000,
                    help='safety bound on instructions printed')
    ap.add_argument('--all-regs', action='store_true')
    a = ap.parse_args()

    data, n, st = load(a.statelog)
    recs = [st.unpack_from(data, i * 72) for i in range(n)]

    hits = [i for i, r in enumerate(recs) if r[0] == a.entry]
    if not hits:
        raise SystemExit('entry $%06x never executed in this statelog' % a.entry)
    if a.nth >= len(hits):
        raise SystemExit('only %d call(s) of $%06x in this statelog'
                         % (len(hits), a.entry))
    start = hits[a.nth]
    sp0 = recs[start][16]          # A7 at entry, just after the BSR push

    end = n - 1
    for i in range(start + 1, min(n, start + a.max + 1)):
        if recs[i][16] > sp0:      # the RTS popped the return address
            end = i
            break
    else:
        end = min(n - 1, start + a.max)

    lo = min(r[0] for r in recs[start:end + 1])
    hi = max(r[0] for r in recs[start:end + 1]) + 8
    dis = disasm(a.image, lo, hi)

    print('# call %d of %d, %d instructions, records %d..%d'
          % (a.nth, len(hits), end - start, start, end))
    for i in range(start, end):
        r, nxt = recs[i], recs[i + 1]
        text = dis.get(r[0], '???')
        if a.all_regs:
            delta = ' '.join('%s=%08x' % (NAMES[k], nxt[k])
                             for k in range(1, 17))
        else:
            delta = ' '.join('%s=%08x' % (NAMES[k], nxt[k])
                             for k in range(1, 17) if nxt[k] != r[k])
        print('$%06x  %-34s %s' % (r[0], text, delta))

main()
