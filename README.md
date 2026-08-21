# Lotus Turbo Challenge 2 — native

A native port of *Lotus Turbo Challenge 2* (Amiga CD32 / AGA, 1991,
Magnetic Fields / Gremlin Graphics).

The shipped binary contains **no emulator**.  The game's 68000 code is
statically recompiled to C ahead of time, and a growing set of routines
has been replaced outright by hand-written native C.  `make no-musashi`
is a build gate that fails if a single emulator symbol reaches the
native executable.

```
./run.sh         # build it and play it
./run.sh debug   # ...starting on the course preview instead
```

One binary: the game with its bezel, and the RE pages a button away.  X
swaps between them, on the keyboard or the pad.  `make play` and `make
debug` do the same thing.

You supply the game.  The retail WHDLoad install lives in
`original/Lotus2CD32/` and is never committed — see *Legal* below.

## How it is built

**The recompiler.**  `tools/m68k2c.py` turns 68000 into C: one `switch`
over the program counter, over a machine struct.  It decodes from the
PCs a real run actually executed, then follows the code by recursive
descent and through jump tables, so the instruction boundaries are
observed rather than guessed.  Cycle costs are measured per EDGE (this
PC to that next PC) from the oracle's own counter, not averaged per
instruction — the difference matters, because the game's interrupt
timing depends on it.

**The oracle.**  Musashi runs the same game beside it and produces
entry/exit RAM and register images.  It is a measuring instrument only;
nothing from `third_party/musashi/` is linked into the native build.

**The gates.**  Nothing is accepted because it looks right.

| gate | what it demands |
| --- | --- |
| `make recomp-gate` | 28 recompiled routines, register- and memory-exact against the oracle |
| `make frame-gate` | 30 checkpoints through a full boot-and-race, pixel-identical |
| `make render-gate` | the native compositor's frame appears verbatim inside the oracle's raster |
| `make course-gate` | all eight courses reached and compared |
| `make override-check` | whether a routine may be replaced at all: entered only by BSR/JSR, and every register it changes reproduced |
| `make no-musashi` | no emulator symbols in the native binary |

**The engine.**  `src/engine/` is the ported game: the road pipeline
(sky bands, keyframe generator, edge interpolator, perspective pass,
blit queue, band blitter), the car model, the input decoder, a blitter,
and a per-line copper compositor.  Each routine carries the address it
came from and the snapshot that proves it.

## The RE pages

`make debug` gives the game plus five pages.  The COURSE page runs the
game's own road chain over a race snapshot and seeks it by writing the
course position long at `$30d8(A3)` — the same record index the game's
generator walks the course table by — so dragging the scrub bar drives
the real interpolator, beside a top-down map, a gradient profile and a
whole-course strip read from the same table.

## Some things learned the hard way

* `move.w` leaves the top half of a data register alone; an address
  register as destination is always 32-bit and sets no flags.  One
  `addq.w #6,A0` treated as a word cost five gates.
* `asr` is arithmetic.  Implemented as a logical shift on an unsigned
  value it drops every other bit, and the oracle diff looks like
  `expected AND $55`.
* A read-modify-write through `(An)+` adjusts the pointer once, not
  twice.
* Decode images must be coherent per memory region.  A stale chip-RAM
  image still disassembles — into confident nonsense.
* A routine can be byte-exact and still not be replaceable.  Charging a
  fixed cycle cost for a routine whose real cost varies per course
  (`car_update`: 970 cycles in FOREST, 768 in STORM) breaks the frames
  it used to pass.

## Layout

```
src/host/      chipset, WHDLoad shim, front ends
src/recomp/    generated C + the runtime it runs on
src/engine/    routines ported to native C
src/viewer/    the RE pages
tools/         recompiler, gates, capture and analysis
re/            what the reverse engineering found
third_party/   Musashi (oracle only)
```

## Legal

The code here is mine.  The game is not.  The retail slave, `Disk.1` and
everything extracted from them stay out of this repository; `.gitignore`
enforces it.  Bring your own copy.
