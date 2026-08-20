#!/usr/bin/env python3
"""track_dump.py -- decode the Lotus 2 course layout from an ExpMem dump.

The course lives at A3-$1e7d ($206183) as 16-byte records.  The generator
at $213edc walks it with `adda.w #$10,a0` then reads `(-$b,a0)` and
`(-$a,a0)`, i.e. record byte 5 = curvature delta, byte 6 = slope (hill)
delta.  The current position is the HIGH word of the long at $30d8(A3)
(the code does `swap d0; asl.w #4` before indexing).

usage: track_dump.py FAST.bin [--map OUT.svg] [--csv OUT.csv]
"""
import argparse
import itertools
import math
import struct

A3 = 0x208000
FAST_BASE = 0x200000
TRACK_BASE = A3 - 0x1e7d
RECORD = 0x10
CURVE_OFF = 5     # record byte read as (-$b,a0) after adda #$10
SLOPE_OFF = 6     # record byte read as (-$a,a0)


def load(path):
    with open(path, "rb") as f:
        return f.read()


def sbyte(mem, addr):
    v = mem[addr - FAST_BASE]
    return v - 256 if v > 127 else v


COURSE_SEGMENTS = 1024   # measured: record 1024 is the first non-course row


def read_track(mem, limit=COURSE_SEGMENTS):
    """Return [(curve, slope)] for the course.

    The table is exactly 1024 records: record 1024 at $20a183 is the first
    row whose bytes leave the +-8 curvature range the road generator can
    represent (it holds $0240/$0ccc-style words, i.e. the next asset), and
    the last 2 course records are an all-zero run-out.
    """
    out = []
    for i in range(limit):
        rec = TRACK_BASE + i * RECORD
        if rec - FAST_BASE + RECORD > len(mem):
            break
        out.append((sbyte(mem, rec + CURVE_OFF), sbyte(mem, rec + SLOPE_OFF)))
    return out


def current_segment(mem):
    d0 = struct.unpack_from(">I", mem, A3 + 0x30d8 - FAST_BASE)[0]
    return (d0 >> 16) & 0xffff


def centreline(track):
    """Integrate curvature into an x/y path, the way the road walks it."""
    x = y = 0.0
    heading = 0.0
    path = [(0.0, 0.0)]
    for curve, _slope in track:
        heading += curve * 0.012      # scale chosen so a course fits a page
        x += math.sin(heading)
        y += math.cos(heading)
        path.append((x, y))
    return path


def write_svg(path_points, out, marker=None):
    xs = [p[0] for p in path_points]
    ys = [p[1] for p in path_points]
    minx, maxx = min(xs), max(xs)
    miny, maxy = min(ys), max(ys)
    pad = 20
    w = maxx - minx or 1
    h = maxy - miny or 1
    scale = 900 / max(w, h)
    def tx(p):
        return (p[0] - minx) * scale + pad, (maxy - p[1]) * scale + pad
    pts = " ".join(f"{a:.1f},{b:.1f}" for a, b in map(tx, path_points))
    width = w * scale + pad * 2
    height = h * scale + pad * 2
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:.0f}" '
        f'height="{height:.0f}" viewBox="0 0 {width:.0f} {height:.0f}">',
        f'<rect width="100%" height="100%" fill="#12141a"/>',
        f'<polyline points="{pts}" fill="none" stroke="#5ac8fa" '
        f'stroke-width="6" stroke-linejoin="round" stroke-linecap="round"/>',
    ]
    sx, sy = tx(path_points[0])
    parts.append(f'<circle cx="{sx:.1f}" cy="{sy:.1f}" r="9" fill="#3ddc84"/>')
    if marker is not None and marker < len(path_points):
        mx, my = tx(path_points[marker])
        parts.append(f'<circle cx="{mx:.1f}" cy="{my:.1f}" r="9" '
                     f'fill="#ffd60a"/>')
    parts.append("</svg>")
    with open(out, "w") as f:
        f.write("\n".join(parts))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fast")
    ap.add_argument("--map")
    ap.add_argument("--csv")
    args = ap.parse_args()

    mem = load(args.fast)
    track = read_track(mem)
    here = current_segment(mem)

    print(f"course: {len(track)} segments, car at segment {here}")
    runs = [(k, len(list(g))) for k, g in itertools.groupby(track)]
    print(f"{len(runs)} constant-curvature runs")
    bends = [(c, n) for (c, s), n in runs if c]
    left = sum(n for c, n in bends if c < 0)
    right = sum(n for c, n in bends if c > 0)
    hills = sum(1 for c, s in track if s)
    print(f"  {right} segments turning right, {left} left, {hills} with slope")
    print("  first 12 runs:")
    for (c, s), n in runs[:12]:
        kind = "straight" if c == 0 else ("right" if c > 0 else "left")
        print(f"    {kind:8s} curve={c:3d} slope={s:3d}  x{n}")

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("segment,curve,slope\n")
            for i, (c, s) in enumerate(track):
                f.write(f"{i},{c},{s}\n")
        print("wrote", args.csv)
    if args.map:
        write_svg(centreline(track), args.map, marker=here)
        print("wrote", args.map)


if __name__ == "__main__":
    main()
