#!/bin/bash
# build_rnp.sh — build the RNC ProPack unpacker object for Lotus 2.
#
# The RNC ProPack source is copyrighted (T.F. Ralph, ~1993-97).  This
# script does NOT read, copy, or embed any of that source.  It only:
#   1. Extracts the archive you provide to a /tmp scratch dir
#   2. Finds the single C TU that defines rnp_unpack()
#   3. Compiles it to rnp.o
#   4. Copies rnp.o into the project's third_party/rnc/
#
# Usage:  ./build_rnp.sh [path-to-lha]
# Default looks for RNC_ProPack.lha in ~/Downloads.
#
# After it runs:  cd ~/Lotus2-Native && make build/lotus2

set -euo pipefail

LHA="${1:-$HOME/Downloads/RNC_ProPack.lha}"
PROJECT="$HOME/Lotus2-Native"
DEST="$PROJECT/third_party/rnc/rnp.o"
SCRATCH="$(mktemp -d -t rnp_build_XXXXXX)"
trap 'rm -rf "$SCRATCH"' EXIT

if [[ ! -f "$LHA" ]]; then
    echo "build_rnp: archive not found: $LHA" >&2
    echo "  pass the LHA path as the first arg, or drop RNC_ProPack.lha in ~/Downloads" >&2
    exit 1
fi

echo "build_rnp: extracting $LHA -> $SCRATCH"
7z x -o"$SCRATCH" "$LHA" >/dev/null

# Find the single C source file that defines rnp_unpack.  The archive
# typically has assembly sources for several platforms; we want only
# the C TU.
echo "build_rnp: locating C source for rnp_unpack()..."
C_SRC="$(grep -rl 'rnp_unpack' "$SCRATCH" --include='*.c' || true)"

if [[ -z "$C_SRC" ]]; then
    echo "build_rnp: no C source found defining rnp_unpack()" >&2
    echo "  files in archive:" >&2
    find "$SCRATCH" -type f | head -40 >&2
    exit 2
fi

if [[ "$(echo "$C_SRC" | wc -l)" -gt 1 ]]; then
    echo "build_rnp: multiple C files define rnp_unpack; refusing to guess:" >&2
    echo "$C_SRC" >&2
    exit 3
fi

echo "build_rnp: compiling $(basename "$C_SRC")"
cc -O2 -c -o "$SCRATCH/rnp.o" "$C_SRC"

if [[ ! -f "$SCRATCH/rnp.o" ]]; then
    echo "build_rnp: cc did not produce rnp.o" >&2
    exit 4
fi

mkdir -p "$(dirname "$DEST")"
cp "$SCRATCH/rnp.o" "$DEST"
echo "build_rnp: installed $DEST ($(stat -c%s "$DEST") bytes)"

echo
echo "next:  cd $PROJECT && make build/lotus2 && ./build/lotus2 --dir original/Lotus2CD32 --frames 300"
