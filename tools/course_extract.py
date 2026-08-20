#!/usr/bin/env python3
"""course_extract.py -- pull a course out of an ExpMem snapshot into a
standalone file the debug viewer opens without booting the game.

The viewer needs the 1024 x 16-byte course table at $206183 plus the
palette the course is drawn with; sampling the palette from an oracle
screenshot keeps the preview wearing the game's own colours.

usage: course_extract.py FAST.bin OUT.l2c [--frame SHOT.ppm] [--name NAME]
"""
import argparse
import struct
import sys

FAST_BASE = 0x200000
A3 = 0x208000
TRACK_BASE = A3 - 0x1e7d
SEGMENTS = 1024
RECORD = 0x10
MAGIC = b"L2C1"
GAME_OX, GAME_OY = 17, 18      # race window in the host raster (gate-measured)


def read_ppm(path):
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        sys.exit(path + ": not a P6 PPM")
    fields, i = [], 2
    while len(fields) < 3:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i] != 0x0A:
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _ = fields
    return w, h, data[i:i + w * h * 3]


def sample(shot, x, y):
    """True RGB at a game-window pixel.

    The host's write_ppm emits blue, green, red in that order (its rgb4()
    packs blue in the high byte and write_ppm walks high to low), so the
    stored triple has to be reversed to get real RGB.
    """
    w, h, pix = shot
    o = ((GAME_OY + y) * w + GAME_OX + x) * 3
    b, g, r = pix[o], pix[o + 1], pix[o + 2]
    return (r, g, b)


def dominant(shot, y0, y1, pick):
    """Most common colour in a band that satisfies `pick`."""
    from collections import Counter
    c = Counter()
    for y in range(y0, y1):
        for x in range(0, 320, 2):
            col = sample(shot, x, y)
            if pick(col):
                c[col] += 1
    return c.most_common(1)[0][0] if c else (0, 0, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fast")
    ap.add_argument("out")
    ap.add_argument("--frame")
    ap.add_argument("--name", default="")
    args = ap.parse_args()

    mem = open(args.fast, "rb").read()
    off = TRACK_BASE - FAST_BASE
    table = mem[off:off + SEGMENTS * RECORD]
    if len(table) != SEGMENTS * RECORD:
        sys.exit("snapshot too small for the course table")

    if args.frame:
        shot = read_ppm(args.frame)
        # sky: the dominant blue up top; grass: the dominant green in the
        # lower half; tarmac: the dominant neutral grey down there too
        sky = dominant(shot, 40, 90, lambda c: c[2] > c[0] and c[2] > c[1])
        grass = dominant(shot, 130, 199,
                         lambda c: c[1] > c[0] + 8 and c[1] > c[2] + 8)
        road = dominant(shot, 150, 199,
                        lambda c: abs(c[0] - c[1]) < 12 and
                                  abs(c[1] - c[2]) < 12 and 20 < c[0] < 130)
    else:
        sky, grass, road = (40, 60, 170), (28, 92, 40), (56, 56, 60)

    h = 2166136261
    for i in range(SEGMENTS):
        for b in (table[i * RECORD + 5], table[i * RECORD + 2]):
            h ^= b
            h = (h * 16777619) & 0xffffffff

    with open(args.out, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<II", SEGMENTS, h))
        f.write(args.name.encode()[:31].ljust(32, b"\0"))
        for c in (sky, grass, road):
            f.write(bytes(c))
        f.write(b"\0")
        f.write(table)
    print("%s: %d segments, id $%08x, sky %s grass %s road %s"
          % (args.out, SEGMENTS, h, sky, grass, road))


if __name__ == "__main__":
    main()
