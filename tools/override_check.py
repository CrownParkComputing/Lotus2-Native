#!/usr/bin/env python3
"""Say whether a routine may be replaced by a native override.

Two conditions, both static, both cheap, and both learned the hard way:

  CALL-ONLY ENTRY.  An override finishes with the RTS the instructions
  would have done, so it must only ever be entered by BSR/JSR.  $2160f2
  is also reached by four BRAs and two conditional branches; on those
  entries there is no return address, and popping one corrupts the stack.
  The damage is small and path-dependent, which is the worst kind.

  MODELLED REGISTERS.  The C port must reproduce every register the
  original changes, not just its effect on memory -- `make render-gate`
  reports that half and marks a stage "[not override-eligible]".

Usage: python3 tools/override_check.py 21263c 215adc 2169e0 ...
"""
import subprocess, re, sys

IMAGE = 're/pipeline/decode.bin'
LO, HI = 0x200, 0x390000

def entries(targets):
    out = subprocess.run(['./build/dasm', IMAGE, hex(0x20c000), hex(0x218200)],
                         capture_output=True, text=True).stdout
    out += subprocess.run(['./build/dasm', IMAGE, hex(0x200), hex(0x80000)],
                          capture_output=True, text=True).stdout
    hits = {t: {} for t in targets}
    for line in out.splitlines():
        m = re.match(r'\$([0-9a-f]{6})\s+(\S+)\s+\$([0-9a-f]+)', line)
        if not m:
            continue
        mnem = m.group(2).split('.')[0]
        tgt = int(m.group(3), 16)
        if tgt in hits:
            hits[tgt][mnem] = hits[tgt].get(mnem, 0) + 1
    return hits

CALLS = ('bsr', 'jsr')
targets = [int(a, 16) for a in sys.argv[1:]]
if not targets:
    raise SystemExit(__doc__)
hits = entries(targets)
bad = 0
for t in targets:
    kinds = hits[t]
    non_call = {k: v for k, v in kinds.items() if k not in CALLS}
    calls = sum(v for k, v in kinds.items() if k in CALLS)
    if non_call:
        bad += 1
        print("$%06x  NOT overridable: entered by %s (and %d call%s)"
              % (t, ', '.join('%d x %s' % (v, k) for k, v in sorted(non_call.items())),
                 calls, '' if calls == 1 else 's'))
    elif calls == 0:
        print("$%06x  no entries found -- check the scanned range" % t)
    else:
        print("$%06x  call-only (%d BSR/JSR): entry condition satisfied"
              % (t, calls))
sys.exit(1 if bad else 0)
