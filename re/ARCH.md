# Lotus Turbo Challenge 2 (CD32) — architecture notes (measured)

Kernel family: **phase-direct** (no Sales Curve verb tables; proven absent).
Sources: `re/pipeline/disasm/decomp.c` (Ghidra, A3 pinned to $208000),
`re/pipeline/disasm/slave_disasm.txt` (IRA), `re/pipeline/statelog*.bin`
(18 x u32 LE per record: pc, d0-d7, a0-a7, sr), `re/pipeline/*gameplay.bin`
(mid-race RAM), `re/pipeline/objlog.txt`, `re/pipeline/handlers.txt`.
Listing addresses below are runtime addresses (fast.bin base $200000,
chip.bin base $0, slave base $380000).

## Registers / globals

- **A3 = $208000 always** (99.6% of 3.8M gameplay records). `N(A3)` = global;
  the base page spans at least $2b00-$3400.  Verified mid-race:
  `tst.w $2fee(A3)` is the frame-park instruction.
- **A6 = $dff000** inside render/IRQ code (custom-chip base idiom).
- A2 = $1400 (97.3% attract) — chip-side scratch base; in gameplay loops A2
  is repurposed as the zoom-table pointer ($204428).
- Base-page fields (measured; names provisional):
  - `$2fc0.w` phase index for the VBLANK param table at `$20de12`
  - `$2fd6.l / $2fda.l` screen buffers ($10186 / $1bd06 chip, swapped by
    `$20f69e`); `$2fde.w` related flag
  - `$2fe0.w` phase ($11/$13 frame phases; $18, $34, $10 sequence steps)
  - `$2fe6.w` attract frame counter; `$2fe8/$2fea/$2fec.w` phase params
  - `$2fee.w` frame tick (set by VBLANK path; main loop parks on it)
  - `$2ff0.w` exit flag (checked in every park spin)
  - `$2fa4.w / $2fa8.w` blitter-queue watermark pair (pacing spin at
    `$212c9e` waits for difference >= 2)
  - `$3000.w` palette bank selector (fade builder target = $5400 + bank*$40)
  - `$304a/$304e/$3050.w` sequence step vars
  - `$320c..` 8x8 font glyph cache (the "LOTUS TURBO CHALLENGE II" glyphs
    appear here as raw bitmaps)

## Memory map (post-install, mid-race)

- `$200-$fff` — decrunched install body (RNC2 blob from the slave);
  exports a service routine called via `jsr $204.w` (trampoline $20d2a0)
- `$1400` — chip scratch base (A2 in attract)
- `$5400` — 16-palette-bank workspace, 64 bytes/bank
- `$10186 / $1bd06` — double-buffered screens (320x200, plane stride $1f40)
- `$62b74-$63xxx` — Imagitec ProTracker replay: song data base $63544,
  command table $62826, hooks $7d4c4/$7d4c8
- `$644xx` — chip road-keyframe table (walked downward by A0)
- `$7f5f0 / $7fedc` — copper lists (COP1LC)
- `$7ff22+` — copper list under construction (verb A writes BPLCON0 +
  bitplane pointers here)
- `$200000+` — ExpMem: code $20d294-$217xxx, then data
  - `$204272` — scanline nibble table (sky/road colour bands)
  - `$204428` — zoom/perspective lookup (`$40(a2,d3.w)`, 4-byte entries)
  - `$205d88 / $206188` — scanline edge buffers (road left/right per line)
  - `$20cc94` — word table walked downward (A0, stride 2) by the
    interpolator inner loop
  - `$20ce94` — longword stride table indexed by segment ($20ce94+d*4)
  - `$216fb2` — BLTSIZE/stride table for the road blitter driver

## Interrupt map

- Level 3 VERTB = `$20f6c0`; level 2 = `$20f5aa`; level 5 BLIT = `$20d91a`
  (vectors installed by the sequencer: `move.l a0,$6c.w` etc.)
- VBLANK tail `$20ddd4`: save-all; call *$7d4c4 (PT command interface,
  d0=0 then d0=4 with d1=phase-table word); store a0 -> $7d4c8; call
  *$7d4c8 (music player tick $62ca8); rte at $20f7d2 returns into the
  parked main loop (flag $2fee(A3) set on the way).

## Main loop (attract/title sequencer, $20dedc-$20e1c0)

Phase-driven: writes phase+params into the base page, parks on
$2fee(A3) (two park loops, $20df86 and $20dfd6/$20e15c/$20e1ac per
phase), per wake runs: palette fade build ($2102ca), buffer swap
($20f69e), screen verbs ($210296/$210272), phase teardown/setup BSRs.
`bne $20e1c0` on $2ff0(A3) = leave the sequence.  Loops back via
`bra.w $20dedc`.

## Gameplay (racing) core

- Road interpolator: inner loop $2142a4-$21433e inside the road blitter
  driver FUN_00213534 ($213534-$214726).  Per segment: A4 walks 8-byte
  keyframe records downward from $206184, A0 walks words downward from
  $20cc94; interpolates between keyframes (cmp/sub/bmi at $2142c0-),
  perspective via $204428 table, emits into scanline edge buffers.
  Trace-verified strides: A0 -2/iter, A4 -8/iter.
- The same function programs the blitter per band: BLTAPT ($dff050),
  BLTDPT ($dff054), BLTCON0 ($dff040), BLTSIZE ($dff058 = $95..),
  BBUSY poll on DMACONR ($dff002 bit 6).  A6=$dff000 confirmed in trace.
- Frame pacing: NOP-sled spin at $212c9e until $2fa4(A3)-$2fa8(A3) >= 2
  (blitter queue watermark), toggling INTENA bit 1.

## Sound

Imagitec ProTracker replay (string at $6273e).  Commands via dispatcher
$62810 (d0 = command 0-8, d1 = param; handlers in `handlers.txt`).
Music is ProTracker-format MOD data at $63544(pc)-relative structures;
cookbook asset route: extract MOD, replay via libxmp.

## Known host caveats

- `--no-video` stalls the frame tick (issues/known-issues.toml
  #lotus2-no-video-stalls-game): captures must run with video on.
- Video self-test: early-fetch fine-scroll lead failure (inherited from
  the SWIV host; check at PARITY).
