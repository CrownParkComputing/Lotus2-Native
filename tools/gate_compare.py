#!/usr/bin/env python3
"""gate_compare.py -- pixel-exact gate: find the native 320x200 frame inside
the oracle's 352x288 raster and demand byte equality.

usage: gate_compare.py ORACLE.ppm NATIVE.ppm

Exit 0 = the native frame appears verbatim in the oracle raster (offset
printed); exit 1 = no offset gives an exact match (best mismatch reported).
"""
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        sys.exit(f"{path}: not a P6 PPM")
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
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
    i += 1  # single whitespace after maxval
    w, h, _ = fields
    return w, h, data[i:i + w * h * 3]


def main():
    ow, oh, opix = read_ppm(sys.argv[1])
    nw, nh, npix = read_ppm(sys.argv[2])
    orows = [opix[y * ow * 3:(y + 1) * ow * 3] for y in range(oh)]
    nrows = [npix[y * nw * 3:(y + 1) * nw * 3] for y in range(nh)]

    best = (None, -1)  # (offset, matching rows)
    for oy in range(oh - nh + 1):
        start = 0
        while True:
            ox3 = orows[oy].find(nrows[0], start)
            if ox3 < 0:
                break
            start = ox3 + 3
            if ox3 % 3:
                continue
            ox = ox3 // 3
            rows = 0
            for y in range(nh):
                if orows[oy + y][ox3:ox3 + nw * 3] != nrows[y]:
                    break
                rows += 1
            if rows == nh:
                print(f"EXACT match at oracle offset ({ox},{oy})")
                return 0
            if rows > best[1]:
                best = ((ox, oy), rows)
    print(f"NO exact match; best candidate {best[0]} matched "
          f"{best[1]}/{nh} rows")
    return 1


if __name__ == "__main__":
    sys.exit(main())
