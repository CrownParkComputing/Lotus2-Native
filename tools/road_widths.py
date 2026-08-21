#!/usr/bin/env python3
"""Measure the road's cross-section out of the game's own strip art.

`road_bands()` draws each screen line by blitting a strip chosen by a
width index.  The strips are 1-bit masks: a solid run, then a GAP, then
solid again -- the gap is the road, and how wide it is IS the road's
width at that distance.  So the geometry a vector renderer needs is not
hidden in the code, it is measurable from the art in one pass.

  python3 tools/road_widths.py re/pipeline/road/ph_0_211e78_fast.bin \
                              re/pipeline/road/ph_0_211e78_chip.bin
"""
import struct, sys

A3 = 0x8000
TABLE = A3 + 0x4e94          # $4e94(A3): longs, one strip pointer each


def widths(fast, chip, first=0, last=64):
    out = []
    for idx in range(first, last):
        p = struct.unpack_from('>I', fast, TABLE + idx * 4)[0]
        if not p or p + 160 > len(chip):
            continue
        bits = [(b >> (7 - k)) & 1 for b in chip[p:p + 160] for k in range(8)]
        if 0 not in bits:
            continue
        start = bits.index(0)
        end, run = start, 0
        for i in range(start, len(bits)):
            if bits[i] == 0:
                end, run = i, 0
            else:
                run += 1
                if run >= 24:      # a long solid run ends the gap
                    break
        out.append((idx, start, end - start + 1))
    return out


def main():
    fast = open(sys.argv[1], 'rb').read()
    chip = open(sys.argv[2], 'rb').read()
    print("idx  gap starts at  road width (px)")
    for idx, start, w in widths(fast, chip):
        print("  %2d      %4d           %3d" % (idx, start, w))


main()
