#!/bin/sh
# run.sh -- build the all-in-one app and start it.
#
# One binary: the game with its bezel, and the debug pages a button away.
# X (keyboard or pad) swaps between them.
#
#   ./run.sh              start on the game
#   ./run.sh debug        start on the course preview instead
#   ./run.sh courses      same thing
#   ./run.sh graphics     start on the chip-RAM picture viewer
#   ./run.sh sound        start on the Paula voices
#   ./run.sh --dir DIR    point at a different WHDLoad install
#
# Anything else is passed straight through to the binary.
set -e
cd "$(dirname "$0")"

INSTALL=${LOTUS2_INSTALL:-original/Lotus2CD32}
PAGE=
ARGS=

while [ $# -gt 0 ]; do
    case "$1" in
        debug|courses|course) PAGE=COURSE ;;
        graphics|gfx)         PAGE=GFX ;;
        sound|music)          PAGE=SOUND ;;
        game)                 PAGE=GAME ;;
        --dir)                INSTALL=$2; shift ;;
        *)                    ARGS="$ARGS $1" ;;
    esac
    shift
done

if [ ! -d "$INSTALL" ]; then
    echo "run.sh: no WHDLoad install at $INSTALL" >&2
    echo "        put the game there, or set LOTUS2_INSTALL." >&2
    exit 1
fi

# The recompiled C is generated, not committed, and the object it
# compiles to takes a while; make only rebuilds what has changed.
make build/lotus2_play

exec ./build/lotus2_play --live --bezel --dir "$INSTALL" \
     ${PAGE:+--page $PAGE} $ARGS
