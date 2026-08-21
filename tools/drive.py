#!/usr/bin/env python3
"""Build an input recording from a script, run it, and show the result.

Reaching courses 2-8 needs the password screen, and no fixed fire pattern
navigates a menu.  But the input format is one byte per frame, and the
screen can be read back, so the menus can be driven deliberately:
describe the presses, run them, look at the frame, adjust.

  python3 tools/drive.py out.rec 6000 "2200:fire" "2400:down" ...

Bits: 1 up, 2 down, 4 left, 8 right, 16 fire.  A press is held for
`hold` frames (default 6), which is long enough for the game to see it
and short enough not to auto-repeat.
"""
import subprocess, sys

BITS = {'up': 1, 'down': 2, 'left': 4, 'right': 8, 'fire': 16}

def build(path, frames, presses, hold=6):
    buf = bytearray(frames)
    for spec in presses:
        at, _, what = spec.partition(':')
        at = int(at)
        bits = 0
        for w in what.split('+'):
            w = w.strip()
            if w in BITS:
                bits |= BITS[w]
            elif w.startswith('hold'):
                pass
        for f in range(at, min(frames, at + hold)):
            buf[f] |= bits
    open(path, 'wb').write(buf)
    return len(buf)

if __name__ == '__main__':
    out, frames = sys.argv[1], int(sys.argv[2])
    n = build(out, frames, sys.argv[3:])
    print("wrote %s (%d frames)" % (out, n))
