#!/bin/bash
# assemble_rnp.sh -- assemble the RNC ProPack MC68000 port to a linkable
# .o file.  The RNC ProPack source is copyrighted (T.F. Ralph / Rob
# Northen Computing); this script never reads, copies, or embeds any of
# that source.  It just runs `vasm` on whichever .S file the user points
# it at, producing an object file the project links but never sees the
# source of.
#
# Usage:  assemble_rnp.sh [path-to-rnc2.S]
# Default looks for SOURCE/MC68000/RNC_2.S under a path derived from
# RNC_PROPACK_SRC (which the user sets to the extracted archive).

set -euo pipefail

if ! command -v vasm >/dev/null 2>&1; then
    echo "assemble_rnp: vasm not found in PATH" >&2
    echo "  install with:  sudo pacman -S vasm" >&2
    echo "  (or apt:  sudo apt install vasm)" >&2
    exit 1
fi

RNC_PROPACK_SRC="${RNC_PROPACK_SRC:-$HOME/Downloads/RNC_ProPack_extracted}"
S_FILE="${1:-$RNC_PROPACK_SRC/SOURCE/MC68000/RNC_2.S}"

if [[ ! -f "$S_FILE" ]]; then
    echo "assemble_rnp: source file not found: $S_FILE" >&2
    echo "  pass the path as the first arg, or set RNC_PROPACK_SRC" >&2
    exit 1
fi

DEST="$(cd "$(dirname "$0")/.." && pwd)/third_party/rnc/rnp_68k.o"
mkdir -p "$(dirname "$DEST")"

echo "assemble_rnp: vasm -m68000 -Fhunk -o $DEST $S_FILE"
vasm -m68000 -Fhunk -o "$DEST" "$S_FILE"
echo "assemble_rnp: installed $DEST ($(stat -c%s "$DEST") bytes)"
