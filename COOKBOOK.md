# COOKBOOK for this project (as run on Lotus Turbo Challenge 2 CD32, 2026-08-19)

The general pipeline lives at
`/home/jon/recomp-cookbook/cookbook/RECOMP_COOKBOOK.md`; the SWIV walk-
through that this follows is at `/home/jon/Uridium2-Native/COOKBOOK.md`
("as run on SWIV, 2026-08-19").  This file records what was actually
done on Lotus 2 in this session, what is still open, and what is
definitely *not* settled.

## Stages we touched

### INGEST — done

* `cp -r /tmp/lotus2/Lotus2CD32/* original/Lotus2CD32/`
* The retail install is structured: the slave, `Disk.1` (958468 bytes),
  `ReadMe`, `Manual`, `Message`, `Codes`, plus `.info` siblings.
* `original/` is gitignored; nothing in it is committed.

### ORACLE — unblocked (2026-08-20)

* Vendored Musashi at `third_party/musashi/` (from
  `BattleSquadron/src/musashi/`).
* Copied the SWIV-Amiga host (`src/host/amiga.c`, `amiga.h`, `whdload.c`,
  `whdload.h`) unchanged; the chipset emulation is title-neutral.
* Added a runner `src/host/lotus2_run.c` with Lotus 2-specific defaults
  (slave name, install path, `--ppm-seq` / `--ppm-every` / `--trace`).
* Wrote a `resload_Decrunch` handler in `src/host/whdload.c` that calls
  the self-contained RNC2 unpacker in `src/host/rnc2.c`.
* The RNC2 decoder that previously returned `rc=-5` was replaced with a
  faithful port of the method-2 unpack path from the public RNC ProPack
  decompilation (lab313ru/rnc_propack_source).  Verified standalone
  against the install blob (slave file offset `$10CA`): exactly 4096
  bytes out, header CRC16 `$AB5A` matches, output is valid 68000 code.
  See `issues/known-issues.toml#lotus2-rnc2-decoder` (closed).
* Boot after the fix:
  * `resload_DiskLoad disk 1 $0+$400 -> $070000` (1024 bytes from the
    head of `Disk.1`).
  * `resload_CRC16 $070000+$400` returned `$418e`, matching the slave's
    install-version switch (install 1.13 by JOTD).
  * `resload_Decrunch $3810aa -> $0200` now succeeds
    (`packed=1610 unpacked=4096/4096 rc=0`).
  * The install body runs: 122 further DiskLoads (~546 KB) pull the
    whole game from `Disk.1` into ExpMem, 27 patch commands apply, and
    frames render (bplcon0=$1200, copper active, pixels growing every
    frame — 125580 pixels over a 300-frame boot).

### base-detect — done (2026-08-20)

* Re-ran the oracle for 2000 frames with `SWIV_STATELOG` +
  `SWIV_STATELOG_FROM=1500` + `SWIV_STATELOG_MAX=4000000` (steady state:
  main game code, PC alternating between chip `$07dxxx` and ExpMem
  `$20dxxx`) plus the three RAM dumps from the project manifest
  (`slave.bin`, `fast.bin`, `chip.bin`).
* The game reaches its real attract loop: frame 1000 = title screen
  (Lotus badge, TURBO CHALLENGE 2), frame 2000 = blue Esprit screen,
  5 bitplanes, all four Paula channels live with music.
* `base_detect.py --code 62000-212000` over the 4M-record statelog:
  **A3 = $208000, 99.6% dominance** → `re/pipeline/bases.json`.
  Corroborated independently by the frame-2000 next-instruction
  `tst.w ($2fee,A3)`.  `kernel.base = "208000"` in the project JSON.
* Secondary observations: A2 = $1400 (97.3%, a chip-RAM structure —
  candidate for a second base, below the 99% cut); A6 = $dff000 is just
  the custom-chip base idiom, not a game base.

### dispatch-table — done (2026-08-20)

* The `salescurve` gfx-word→handler pattern is **proven absent** from
  both RAM dumps — Lotus 2 is not a verb-table engine.  Do not look
  for one.
* The kernel is **phase-driven, direct-call** (`kernel.family =
  "phase-direct"`):
  * Park loop at `$20df80`: spins on `tst.w $2fee(A3)` (frame tick set
    by VBLANK), `$2ff0(A3)` is the exit flag, then fixed BSRs
    (palette-fade table builder `$2102ca`, double-buffer swap
    `$20f69e`, …).  Two frame phases (double-buffered): one writes
    `$2fe0(A3)=$11`, the other `$13`.
  * VBLANK hook: `$20ddd4` saves regs and calls through two hooked
    pointers in chip RAM, `$7d4c4` / `$7d4c8`, passing a phase-indexed
    word from the table at `$20de12` (index `$2fc0(A3)`; values are
    $30/$38/$40-class display params).
* The only live jump-table dispatcher is the **sound driver's**: chip
  `$062810`, a counted PC-relative jump table
  (`LEA table(PC),A6; CMP.W (A6)+,D0; BLT; RTS; ADD.W D0,D0;
  ADDA.W (A6,D0.W),A6; JMP -$2(A6)`), 9 commands at table `$62826`.
  It belongs to the replay identified by the string at `$06273e`:
  *"ProTracker Replay Routine....(C)Copyright 1991 Imagitec Design"*.
  Commands 0/2/3/4 executed during the 2000-frame attract window.
  New `find_dispatch.py --family imagitec-pt-cmd` preset finds it
  (and rejects the `fast.bin` data false positive); salescurve
  behaviour unchanged (regression-checked against SWIV's amprog.bin).
* Artefact: `re/pipeline/handlers.txt`.
* Asset note for later: music is ProTracker-format MODs replayed by
  the Imagitec routine — the cookbook's "MODs via libxmp" path applies.

### seed-disasm — done (2026-08-20)

* Built `re/pipeline/combined.bin` — one $0-$890000 image holding
  chip.bin at $0, fast.bin at $200000, slave.bin at $380000, so Ghidra
  resolves cross-region calls (e.g. the main loop's `jsr $204.w` into
  the install body, and `jsr $380892.l` into the slave).
* Seeds: `re/pipeline/seeds/pcs.txt` (1931 executed PCs, union of the
  full-run pcset and the statelog window) and `funcs.txt` (61 call
  targets: BSR/JSR successors mined from the statelog + the 9 PT-replay
  command handlers + IRQ vector entries).
* `SeedDisasm.java` now accepts `REG=0xVALUE` (e.g. `A3=0x208000`);
  legacy bare-hex form still pins A6.  Run pinned A3 to the base page.
* Result: `re/pipeline/disasm/decomp.c` (8677 lines) + `funcs.txt`
  (192 functions); IRA listing `re/pipeline/disasm/slave_disasm.txt`.
* Structure recovered while seeding (matters for TRANSLATE):
  * VBLANK (level 3, vector $6c) = `$20f6c0`; level 2 = `$20f5aa`;
    level 5 BLIT = `$20d91a` (installed programmatically, e.g.
    `move.l a0,$6c.w` in the sequencer).
  * The main "loop" is the attract/title sequencer `$20dedc-$20e1c0`:
    parks spinning on `$2fee(A3)`; the flag is set by the VBLANK path,
    which returns via `rte` at `$20f7d2`.  Phase values seen in
    `$2fe0(A3)`: $11/$13 (frame phases), $34, $18 (sequence steps).
  * Screen buffers: `$2fd6(A3)`/`$2fda(A3)` = chip `$10186`/`$1bd06`
    (double-buffered, swapped by `$20f69e`); copper lists at
    `$7f5f0`/`$7fedc` (COP1LC writes).
  * `$20d2a0` = `jsr $204.w` trampoline into the install body — the
    slave's decrunched code supplies a per-call service routine.

### objwalk — done (2026-08-20), phase-direct edition

* `--ptrlog` ported verbatim from SWIV's swiv_run.c into
  `src/host/lotus2_run.c` (title-neutral object/global logger).
* **Gameplay reached**: `--fire-from 2100 --fire-period 100` drives the
  CD32 pad through the title -> "FOREST COURSE" select -> actual racing
  (HUD: MPH, 1ST, score, countdown).  PC coverage jumps 1931 -> 8868
  distinct PCs; `re/pipeline/pcset.txt` now holds the superset.
* Attract-mode walk (`re/pipeline/objlog.txt`, 2000 frames of base-page
  globals): screen buffers live at frame 19, `$2fe6(A3)` is a per-frame
  attract counter (frames ~395-793), phase/flag words `$2fe0-$2ff0`
  drive the sequencer.  **No object pool exists in attract mode** —
  title/car screens are pure sequencer + blitter work.
* Gameplay walk (statelog from frame 5500, ~80 frames, video ON):
  the hot code is the **road interpolator** at `$2142a0`:
  * A4 walks a 5-word-entry table DOWN from `A3+$2e8e` (keyframe
    stack; entries consumed per frame),
  * A0 walks a chip-side keyframe table down from ~$64446
    (values like 31736/-256, 32767/-232 = segment deltas),
  * A1 targets the scanline edge buffers at `$205d88`/`$206188`,
  * A2 = `$204428` is a zoom/perspective lookup (`$40(a2,d3.w)`),
  * frame pacing spins at `$212c9e` on `$2fa4(A3)-$2fa8(A3) >= 2`.
* Conclusion: no salescurve-style object verb table exists in gameplay
  either.  Live state is the road segment stream + phase flags.
  TRANSLATE should model the sequencer + road interpolator directly.

### TRANSLATE — started (2026-08-20)

* Route-B artefacts exist: `re/ARCH.md` (measured architecture),
  `re/VERBS.md` (verb library for the phase-direct family),
  `src/engine/engine.h` (Game base-page model + verbs API),
  `re/PORTING_GUIDE.md` (porting rules + verification loop).
* Verified against the gameplay statelog while writing VERBS.md:
  park/tick handshake, screen swap, palette fade, copper builders,
  blitter-queue pacing spin, interpolator register strides
  (A0 -2/iter from $20cc94, A4 -8/iter from $206184), and the road
  blitter driver programming BLTAPT/BLTDPT/BLTCON0/BLTSIZE with a
  BBUSY poll (A6=$dff000 confirmed inside FUN_00213534).
* Two useful corrections fell out of trace verification: A6 inside the
  road driver is the custom-chip base (not the game base), and the
  per-loop register roles differ from the whole-window histogram
  (A4's hot value there was an aggregate, not the interpolator's).
  VERBS.md records per-loop facts only.
* Next: port the §1/§2 verbs (small, all verified) into
  `src/engine/`, then the sequencer state machine, then the road
  renderer.  The remaining sequencer BSRs are listed at the bottom of
  VERBS.md with trace coverage already captured.

### TRANSLATE / render-gate — verbs ported + pixel-exact frames (2026-08-20)

* Ported the §2 display verbs into `src/engine/engine.c` (`swap_screens`,
  `build_fade`, `build_copper_planes`, `load_palette`) against guest byte
  images (`src/engine/guest.h`).  Correction found while porting:
  `build_copper_planes` takes the plane COUNT in d0 (the decomp loop runs
  d0 times), not planes-1 as VERBS.md first read it.
* All four verbs verify **word-exact** against the frame-2000 dump
  (`./build/lotus2_native --verify-verbs`).  `build_fade` is checked
  self-consistently: bank 8 (step $10) is the identity, so feeding the
  dumped bank 8 back through the formula must reproduce all 9 banks —
  at frame 2000 the base page's $320c already holds the NEXT screen's
  palette, so a naive $320c-in / $5400-out diff false-fails.
* Wrote a native compositor (`src/engine/compositor.c`): a per-line
  copper interpreter with the HOST's timing model, which is the part
  that makes frames pixel-exact:
  * COLOUR writes behind an hp=0 WAIT **backdate to the start of the
    current line** (the host's `color_line_start[index] = rgb` branch in
    `custom_write`); only cop_h>0 writes become mid-line changes.
  * BPLxPT writes take effect the NEXT line (host snapshots
    `render_bplpt` before running the copper); depth/modulos read live.
  * COP1LC/COP2LC/COPJMPx honoured; a jump pauses the copper until the
    next line (`copper_jump1/2` sets `cop_wait_line = cur_line + 1`).
    Lotus 2's gameplay list COPJMP2-hops between per-frame-built
    segments ($7ed0c -> $7ee64 -> $7f100 -> ... at frame 5600) to skip
    the unused tail of its fixed-size per-line colour tables.
  * WAIT compares `(line & 0xff) & mask >= target & mask` and re-arms
    with the beam-wrap bit, exactly like `copper_run_line`.
* Gates (`make gate-capture` once, then `make render-gate`):
  * frame 2000 (title, coplist $7fedc): EXACT at oracle offset (17,18).
  * frame 5600 (mid-race, coplist $7ed0c): EXACT at (17,18) — HUD, road,
    car, clouds, graduated sky, all of it, with zero pixels different.
  * `--fire-from 2100 --fire-period 100` drives the pad into racing for
    the frame-5600 capture, same as the objwalk run.
* Copper lists seen in the gameplay chip dump (scanner in this session):
  $7dc94 (under construction, 856 moves), $7ed0c / $7f5f0 (the
  double-buffered racing pair), $7fed4 (attract).  The title-screen pair
  is $7f5f0 / $7fedc.
* Known channel quirk, kept for parity: host `rgb4()` packs blue into
  the high byte while `write_ppm` reads the high byte as red, so PPM
  captures are R/B-swapped vs the real screen (the blue sky reads as
  red in the PPM).  The native compositor reproduces the same math so
  gates compare byte-for-byte.  Fix BOTH sides together at PARITY if at
  all (cf. the Taito Z palette-swap episode).
* Not modelled yet in the compositor (nothing needed it): sprites,
  BPLCON1 fine scroll, mid-line colour splits, dual playfield, EHB/HAM.

### road pipeline — first two stages ported EXACT (2026-08-20)

* **New host facility: RAM snapshots at arbitrary PCs.**
  `SWIV_SNAP_PCS=hex[,hex...]` (+ `SWIV_SNAP_FROM` frame, `SWIV_SNAP_MAX`,
  `SWIV_SNAP_PREFIX`) dumps registers + full chip + full ExpMem whenever
  a listed PC executes.  Snapshotting the PCs *between consecutive BSRs*
  of a call chain yields an entry/exit RAM pair per stage — i.e. a
  proven input->output transform to port against, which beats reading
  the disassembly and hoping.  `make road-capture` does this for the
  racing chain.  (ExpMem exceeds `amiga_ram()`'s window, so the dump
  goes through `m68k_read_memory_16` rather than a direct pointer.)
* **The racing render tick is a BSR chain at `$212f12`**, found by
  scanning `statelog_gameplay.bin` for control transfers crossing into
  the `$213534-$214730` range (`$2133be -> $213534` etc.).  Stages:
  `$2136f6` (sky/copper bands), `$213eb4`, `$214268` (edge
  interpolator), `$214344`, `$21337c`, `$2143c2` (blit queue),
  `$21508a`.  Per-stage write footprints from the snapshot diffs:

  | stage | writes |
  |-------|--------|
  | `$2136f6` | chip `$7e833+170`, `$7e6a7` (copper band operands) |
  | `$213eb4` | fast `$206169+31` |
  | `$214268` | fast `$205788+334` (the road line stream) |
  | `$214344` | (stack only) |
  | `$21337c` | fast `$205788+334` (second stream) |
  | `$2143c2` | fast `$201f41+71` (blit queue records) |

* **Ported and byte-EXACT** (`./build/lotus2_native --verify-road`, also
  run by `make render-gate`):
  * `road_interpolate` (`$214268`, exits `$214342`) — walks 8-byte
    keyframe records backwards, interpolates edge X with the slope
    tables at `$204428`, emits (colour, x, scale) word-triples per road
    line.  The fixed-point accumulate is the original
    `swap`/`add.l`/`swap`, and D7 steps by the zoom word it stashes at
    `$2ff4(A3)`; both had to be reproduced exactly.
  * `road_blitqueue` (`$2143c2`-`$2144e0`) — appends four record shapes
    to the queue at `$2f42(A3)`, each replaced by a `$ffffffff`
    sentinel when its enable word is zero.  `bls` at `$2144ac` follows
    a `move.w`, which clears C, so it is just `beq`.
* Porting rules that mattered: `adda.w` truncates the addend to 16 bits
  but leaves the high half of the address register alone (open-coded as
  `(a & 0xffff0000) | (uint16_t)(a + x)`), and `mulu.w` promotes to a
  long before the following `add.l`.
* New tool `tools/dasm.c` (`build/dasm IMAGE START END`) disassembles a
  range of `combined.bin` with Musashi — the Ghidra decomp is rough on
  68k, so the listing is the source of truth when porting.

### COURSE TABLE decoded + live viewer (2026-08-20)

* **The course layout is a 1024-record table at `A3-$1e7d` ($206183)**,
  16 bytes per record.  The generator `$213edc` walks it with
  `adda.w #$10,a0` then reads `(-$b,a0)` = **curvature delta** and
  `(-$a,a0)` = **slope (hill) delta**, i.e. record bytes 5 and 6.
  The car's position is the **HIGH word** of the long at `$30d8(A3)` —
  the code does `swap d0; asl.w #4` before indexing, and getting the
  swap backwards (using the low word) reads a dead region of zeroes,
  which is the trap to avoid.
* Record 1024 at `$20a183` is the first row outside the +-8 curvature
  range (it holds `$0240`/`$0ccc`-style words — the next asset), so the
  course is exactly 1024 segments.  Forest course: 239 right-turning,
  222 left-turning, 272 sloped, in 295 constant-curvature runs, with
  ease-out ramps (4,4,...,4,3,2,1,0) at the end of every bend.
* `tools/track_dump.py FAST.bin --csv --map` decodes it, prints the run
  summary and writes an SVG centreline (integrate curvature -> heading
  -> x/y).  `make track` runs it on the road snapshot.
* **`build/lotus2_view` (`make view` / `make view-race`)** is a raylib
  viewer in the SWIV `swivview` house style: fixed logical canvas blitted
  scaled+centred, game view on top, full-width control bar below with
  immediate-mode `button(rect,label,active)` widgets, on-screen page
  buttons rather than keys.  Palette/sizes copied from swivview (bar
  `{28,28,34}`, idle `{50,50,58}`, hover `{80,80,90}`, active
  `{70,130,200}`, border `{120,120,130}`, labels `{255,238,136}`, values
  RAYWHITE, help LIGHTGRAY; 34 heading / 24 button / 20 value / 18 help
  / 16 status).  Pages: PLAY, COURSE, TRACK, GEOM, DISPLAY.
* The COURSE page carries the course work: a **road view that is the
  game's own renderer** (not a debug approximation) with the HUD's top 34
  rows cropped off, a **scrollable/zoomable top-down map**, an elevation
  profile, a scrub bar, and AUTO playback at x0.25/x0.5/x1/x2/x4.
  With DRIVE on, the viewer writes the scrub position into `$30d8(A3)`
  before each emulated frame, so the game's own road pipeline renders
  the scrubbed stretch of course.
* Road edge stream, measured against the real frame: the interpolator's
  triples are `(c, x, z)` per screen line from the horizon line
  (`$30e4(A3)` = 144 mid-race) down to 199.  **Road width on screen =
  `x / 8`** (fits the game frame to ~1% on lines 154-166; below ~180 the
  road is wider than 320 px so the measurement saturates).  `z` is the
  interpolator's D7 line counter in `$100` units.
* `--shot FRAME PATH` / `--page NAME` / `--scrub N` give deterministic
  headless captures; `--shot` also disables mouse input, without which
  stray clicks land on buttons and corrupt the capture.  raylib's
  `TakeScreenshot()` prepends the working directory even to an absolute
  path, so the viewer uses `LoadImageFromScreen()` + `ExportImage()`.

### course record layout, fully mapped (2026-08-20)

Each of the 1024 records at `$206183` is 16 bytes.  Byte roles, measured
by column statistics over the whole course plus the reads in `$213edc`:

| byte | role | evidence |
|------|------|----------|
| 1 | smooth ramped channel, 198 distinct values | continuous curve, not yet named |
| **2** | **gradient (hill)** | ease-ramped 0,1,2,3,4,4,…,4,3,2,1,0; integrates to 18 long hills over the course |
| 3 | mostly -10, ramps at transitions | road width / verge? |
| 4 | repeating down-counter 10..5 | sub-segment index |
| **5** | **curvature** | what `$213edc` reads at `(-$b,a0)`; same ease-ramp shape |
| 6 | per-line vertical delta | what `$213edc` reads at `(-$a,a0)`; alternates sign every few segments |
| **7-14** | **four (object id, x) pairs** | ids `$70`/`$71` always at x = +18/-18 (verge posts); `$82` x1116 (tree), `$84`-`$87` other scenery, `$81`/`$83` rare (signs); x spans about +-64, i.e. +-3.5 road half-widths |

* **The gradient trap.**  Byte 6 is the road *drawer's* per-line vertical
  delta, and integrating it as terrain gives a washboard — the road rides
  up and down every few segments.  Byte 2 is the course's actual gradient
  channel: a single integration of it yields the long smooth climbs and
  descents you drive (Forest: a sustained descent, 18 direction changes
  over 1024 segments).  Do not double-integrate it either; that turns
  every sustained gradient into a parabola.
* The preview also pitch-stabilises against the camera (subtract the
  camera's height and the linear term of its current gradient), so a
  steady slope renders as a steady slope and only *changes* in gradient
  become crests and dips.

### course names are bitmap art, not text (2026-08-20)

* Course names cannot be read out of memory.  Searching the RAM dumps for
  `FOREST`/`MARSH`/`DESERT`/... in ASCII and in shifted-alphabet encodings
  finds nothing, and the 8x8 font glyph cache at base-page `$320c` holds
  **digits only** — two 0-9 sets (indices 8-17 and 28-37) plus a filled
  variant (38-47), no letters.  The HUD's speed/score/time use that cache;
  the course names on the select screen are pre-rendered bitmap graphics.
* So courses are identified by **hashing the course table** (FNV-1a over
  the curvature and slope bytes of all 1024 records).  It is stable and
  unique per course.  `$0ff4d6d5` = FOREST, named from the objwalk route
  that reaches it (title -> "FOREST COURSE" -> race).  Add hashes for the
  other courses by driving to each one once.
* Note also: `re/ARCH.md` calls `$320c` the master palette in one place
  and the font cache in another.  The font cache reading is the correct
  one — `build_fade` verifies EXACT sourcing its palette from fade bank 8,
  not from `$320c`.

### Tier 1 road chain ported (2026-08-20)

Six routines of the race-render chain are now native C and byte-exact
against their snapshot pairs (`./build/lotus2_native --verify-road`, run
by `make render-gate`):

| routine | 68k | size | what it does |
|---|---|---|---|
| `road_sky` | `$2136f6` | 612 instr | sky gradient + horizon copper bands |
| `road_keyframes` | `$213edc` (via `$213eb4`) | ~350 instr | course -> keyframe records |
| `road_interpolate` | `$214268` | ~60 instr | keyframes -> per-line edge stream |
| `road_band_bounds` | `$214354` (via `$214344`) | 28 instr | clipped band bounds |
| `road_perspective` | `$213416` (via `$21337c`) | ~100 instr | perspective pass over the stream |
| `road_blitqueue` | `$2143c2` | ~90 instr | blit descriptor records |

Three findings that cost time and are worth keeping:

* **One course record covers SIXTEEN keyframes, not one.**  `$213edc`
  looks like eighteen unrolled copies of the emit block, but `$2141ec`
  (`subq.w #1,($2f9a,A3); bne $213f5c`) makes the middle sixteen a loop
  body run seven times, and the record load at `$213f5c` sits *outside*
  those sixteen.  So the course advances once per group of sixteen:
  1 + 7x16 + 1 = 128 keyframes from ~9 course records.  Porting it as
  one record per keyframe reads the ease-out ramps far too early and the
  lateral accumulator drifts within twenty records.
* **`$213416` inherits D3 from its caller.**  On the plain path (guard
  `$2dee(A3)` zero) D3 is never initialised, so the extreme it returns
  through `$2eb4(A3)` is whatever the previous stage left in the
  register.  A standalone port has to be fed it; `verify_stage` now
  reads the snapshot's `.regs` sibling and passes registers in.
* **Duff's devices everywhere.**  `$2136f6` has two computed-jump chains
  (50 copies of 10 bytes for the sky, 36 copies of 30 bytes for the
  horizon strip); the entry offset selects how many run.  Port them as
  counted loops: `count = total - (jump_offset / block_size)`.

`verify_stage` now also loads the `_chip.bin` snapshots, so routines that
build copper lists are compared on chip RAM as well as ExpMem.

### Tier 2 done: the band blitter is native (2026-08-20)

`road_bands` ($213534-$21365e) is ported and byte-exact -- the routine
that actually paints the road.  Two supporting pieces landed with it:

* **`src/engine/blitter.c`** -- a native Amiga blitter, ported feature-
  complete from the host's `blit()`: four channels, minterms, both barrel
  shifters, FWM/LWM masks, per-channel modulos, descending mode.  The
  road only needs shifted A->D copies but the rest of the game does not,
  so it was worth porting once properly.
* **Chipset state in snapshots.**  `snapshot_take()` now also writes
  BLTCON0/1, BLTAFWM/BLTALWM, BLTxPT, BLTxMOD, BLTxDAT, DMACON, BPLCON0
  and the 32 colour registers into the `.regs` file.  Those live in
  emulator state, not guest RAM, so without them a native blit could not
  be gated at all.  `verify_stage` loads them into a `Blitter`.

Two 68000 traps cost real time here and are worth remembering:

* **`adda.w` does NOT truncate the result.**  The operand is sign-
  extended to 32 bits and added to the WHOLE address register.  The
  earlier note in this file ("truncates the addend but leaves the high
  half alone") was wrong; `(a & 0xffff0000) | (uint16_t)(a + x)` is a
  miscompilation of it.  Use `a += (int32_t)(int16_t)x`.
* **Writing BLTxPTL masks bit 0.**  Blitter addresses are word aligned
  in hardware (`value & 0xfffe` in the host's `custom_write`).  The
  road's source offset is `line >> 3`, frequently odd, so without the
  mask every other strip reads a byte-misaligned word.  The symptom was
  a clean one-byte shift in the blitted pixels while the run list and
  every ExpMem output stayed exact -- which is the tell that geometry,
  not logic, is wrong.

### car physics located (2026-08-20)

Found with a memory write-watch (`SWIV_WATCH=lo-hi`, already in the host)
rather than by reading code: watch the address, run to gameplay, and the
host reports the first PC that writes it.

The course position is snapshotted through FOUR layers each frame, so the
first three watches only find copies:

    $3054(A3)  <- the live car position, written by the integrator
      -> $305c  ($211058, and $211c1e via the generic object copy)
      -> $30aa  ($211dd4 block)
      -> $30b6  ($212d84)
      -> $30d8  ($212e5c)   <- what the renderer reads

**The integrator is `$2129f2`-`$212ba2`** (149 instructions), called from
`$211e78` and `$211ed4`.  Its core is:

    move.l ($4,A4),D4      ; speed
    move.l ($0,A4),D7      ; position
    move.w ($ce,A4),D0     ; per-frame delta
    ext.l  D0
    bmi    skip            ; a negative delta does not advance
    add.l  D0,D7
    move.l D7,($0,A4)      ; $212a04 -- the root write

A4 is the car/view block at A3+$3054; the delta at +$ce is written a few
instructions earlier ($2129da) from the same routine's D0/D1, so speed and
steering are computed in this routine too.  The second view block is
A3+$3128 (split screen), which is why every field appears twice.

Useful side effect of the watch run: the full list of PCs that write the
view block spans $210fbe-$212bb2, which is the game-logic region -- a
ready-made work list for porting the rest of the car model.

### car model ported (2026-08-20)

`car_update` (`src/engine/car.c`, $2129f2-$212ba2, 149 instructions) is
native and byte-exact.  It does four things in one routine:

* integrates the position: `pos += delta` where the delta is the signed
  word at `+$ce`, skipped entirely when negative;
* accumulates speed from the throttle terms and clamps it to
  `+-$2c90(A3)`, comparing the SWAPPED halves of the long;
* looks up the surface under the car in the course table (`A3-$1e78`,
  field `+$a`, nibble selected by the lateral position) to decide the
  rumble note;
* claims and releases a Paula voice for the engine, and ramps the engine
  note toward its target `$200` per frame.

Audio registers are not modelled yet, so the `$dff000` writes are
no-ops -- but every memory write on those paths is reproduced, which the
gate checks.  The `$20d7e8` voice allocator is not ported; the branch
that calls it is only taken when no voice is held.

The trap that cost the last two bytes: `lsr.w #8,D1` is a LOGICAL shift
of the LOW WORD, so a negative D1 becomes `$00ff`, not `$ffff`, and the
volume clamp then lands on `$40`.  Shifting the 32-bit value instead
gives `$ffff` and the clamp never fires.

### input path found (2026-08-20)

`$211780-$2117ca` is the pad decoder.  It reads the joystick word (D0,
from JOY1DAT) plus the fire button straight off CIA-A PRA
(`btst #$7,$bfe001`), folds them into a bitfield, and publishes three
bytes in the base page:

* `$308a(A3)` -- the live pad state
* `$3086(A3)` -- the previous frame's state
* `$3088(A3)` -- newly-pressed edges, `(prev ^ cur) & cur`

`$308a` is the SAME address as the near car block's control field
(`$36(A4)` with A4 = A3+$3054), so the driving model reads the pad
directly out of its own structure.  Bit 2 is brake, bit 3 accelerate,
bit 4 the second button (gear), tested at `$2127fe`/`$212806`/`$2128d6`.

### car update chain ported (2026-08-20)

Five routines from the near-view chain at `$211e74`, all byte-exact:

| routine | 68k | what |
|---|---|---|
| `car_update` | `$2129f2` | position integration, speed, surface, engine note |
| `car_checkpoint` | `$212680` | checkpoint marker `$7a00`, time top-up, lap flags |
| `car_clock` | `$21263c` | race clock, one unit per `$32` frames |
| `car_distance` | `$212662` | distance/odometer from speed squared |
| `car_shape` | `$212ba4` | road shape under the car from the course record |

`car_drive` ($212734, 230 instructions) and `car_tick` ($21270a) are now
ported too, byte-exact.  `car_drive` is the handling model: suspension
bounce, steering trim from the pad, throttle/brake against the gear
ratio tables, the rev limiter, surface drag, and gear selection (manual
off `$34(A4)` sticky bits, automatic off the rev band `$dac`..`$1388`).

The three table pointers are `$3008` (accelerating), `$300c` (coasting)
and `$3010` (gear ratios); the current gear lives at `+$28`.  Speed is
carried as a 16.16 fraction: every division builds its dividend with
`ext.l` then `swap`, i.e. `speed << 16`.

**68000 division semantics matter here.**  `divu.w`/`divs.w` put the
quotient in the low word and the REMAINDER in the high word, and on
overflow the 68000 sets V and leaves the destination UNCHANGED.  The
game relies on that: the clamps immediately after each divide assume the
old value survived an overflow.  Modelling division as a plain C `/`
that always writes gets subtly different speeds.

### input ported (2026-08-20)

`input_read` (`src/engine/input.c`, $211770-$211850) is native and
byte-exact.  It decodes BOTH ports -- port 1 into `$308a`/`$3086`/`$3088`
and port 0 into `$315e`/`$315a`/`$315c` -- then merges them into `$2faa`
(edges) and `$2fac` (live).  A replay source at `$2f5c`/`$2f5e` can
override port 0, which is how the attract mode drives itself.

The direction decode is the standard Amiga quadrature read: JOYxDAT bits
1 and 9 give right and left directly, and XORing the word with itself
shifted left one bit recovers down and up.  Fire is CIA-A PRA bit 7
(port 1) / bit 6 (port 0), active low.

The engine takes an `Input` struct rather than touching hardware, so the
frontend can drive it from a real pad, a replay, or a test.  The host
snapshot now records `joy0dat`, `joy1dat` and `ciapra` in the `.regs`
file so the decoder can be gated on exactly what the 68000 read.

### the drive loop is complete

input -> car_update -> car_checkpoint -> car_clock -> car_distance ->
car_shape -> car_tick(car_drive) -> road_sky -> road_keyframes ->
road_interpolate -> road_band_bounds -> road_perspective ->
road_blitqueue -> road_bands

Every routine on that path is native C, verified byte-for-byte against
the oracle.  What is still missing for a playable native build is not
physics or rendering but glue: a frame driver that calls them in the
game's order, the sequencer that starts and ends a race, and sound.

### the chains compose (2026-08-20)

Verifying routines one at a time proves each is right; running a whole
chain from one snapshot to a much later one proves they COMPOSE -- that
no routine quietly depends on state a neighbour was supposed to leave.
Both chains are byte-exact end to end:

* `CHAIN car` -- 6 routines, `ph_0_211e78` -> `ph_6_211e98`
* `CHAIN road` -- 6 routines, `st_1_212f12` -> `st_8_212f2e`

Composing surfaced exactly the kind of bug it is meant to: `$213416`
inherits D3, and in a chain that value is not the one in the entry
snapshot -- it is whatever `$214354` left behind two routines earlier.
It turns out to be the clipped bottom edge, which `$214354` also stores
at `view+$98`, so the chain reads it back from memory.  That makes the
dependency explicit rather than relying on a register the caller happens
to still hold, which is what a native engine has to do anyway once the
68000 register file is gone.

### the native engine drives (2026-08-20)

`make drive` runs `build/lotus2_drive`: no 68000, no emulator.  It loads
a mid-race RAM snapshot as the initial state and then runs only ported C
each frame --

    input_read -> car_update -> car_checkpoint -> car_clock ->
    car_distance -> car_shape -> car_tick -> car_frame_latch ->
    car_latch_gap -> race_frame_publish -> road_sky -> road_keyframes ->
    road_interpolate -> road_band_bounds -> road_perspective ->
    road_blitqueue -> road_bands -> composite

Steering and throttle come from the arrow keys/space or a gamepad,
through the ported pad decoder.  The segment counter advances, so the
car is genuinely simulating rather than replaying a still.

Frame-structure routines ported with it, all byte-exact:

* `car_frame_latch` ($211dd4) -- latches both view blocks forward
* `race_frame_begin` ($212cea) -- rotates the game's TRIPLE buffer
  ($1400 / $9742 / $11a84 through $2f8e draw and $2f8a display), cycles
  the 8-entry ring at $3024, swaps the two blit queues
* `race_frame_publish` ($212e58) -- the final position latch to $30d8

**`race_frame_begin` is deliberately NOT called by the demo.**  It is
correct and gated, but rotating the triple buffer only makes sense once
the whole frame is drawn natively: today just the road is, so rotating
shows two frames' worth of stale car and HUD in the other two bitmaps.
Pinning one buffer keeps the demo coherent.  That is a real measure of
what is left -- the buffer rotation becomes usable exactly when the
scenery, sprite and HUD passes are ported.

What the demo does NOT draw natively yet: scenery ($21508a), the car
sprite, the HUD.  What you see of those is left over in the snapshot
bitmap.  Nothing on screen is approximated -- every routine that runs is
byte-exact -- but plenty is simply absent.

### remaining (status 2026-08-20, after the full render-gate pass)

`make render-gate` is green on 24 routines plus both composed chains and
both frame gates.  `$213534` (the band blitter) and the five scenery
iterators (`$215a7a`, `$215a9c`, `$215adc`, `$215b24`, `$215b58`) are
DONE and byte-exact — the engine now carries its own blitter model
(`src/engine/blitter.c`) and the snapshots include the custom chip
registers (`.regs` siblings), which is what unblocked them.

### the scenery pass, measured (2026-08-20)

`$21508a` is not one routine, it is a subsystem.  A call-graph closure
from it (`tools/`-adjacent scratch script, CFG walk over `build/dasm`
output) reaches **33 functions / ~1875 instructions**, six of which are
already ported.  That is comparable in size to the car and road chains
put together, so it is bounded work, not open-ended — but it is not a
single stage.

**`$21508a` itself (the 327-instruction head) is now ported and
byte-exact** as `scen_prepare()` in `src/engine/road.c`, gated against
`sh_0_21508a` -> `sh_1_2151b4` including its A0 and A2 register outputs.

TRAP: `move.w ($30d8,A3),D1` reads the HIGH word of the course-position
long — the whole-record part, not the fraction.  Reading the low word
gets a plausible-looking index and 41 wrong bytes in the copied records.

The head's tail is a two-state scan sharing ONE dbra counter: matching
`$fc00` switches it to hunting `$fd00` and back again.  Which state ran
the counter out decides the exit — falling out of the `$fc00` hunt runs
`$215a7a` next, falling out of the `$fd00` hunt skips that BSR and
publishes a synthetic `$fd00` marker to `$2f2a`.  So the head has to
return a flag, not void.

Below the head the graph layers cleanly, which gives the porting order
(bottom-up, each tier gateable once the tier under it is done):

    tier 0 (leaves, call nothing)   $216aca $216b50 $215dac   [DONE]
                                    $2162dc $216310            (not on FOREST)
    tier 1                          $2169e0                    [DONE]
    tier 2                          $2160f2 -> $216346         [DONE]
                                    (+ $216812/$216916 shadow)
    tier 3                          $215dce  $215ea2
    tier 4 (thin dispatchers)       $215bca/$215bd2  $215c32  $215c60
                                    $215d1e  $2159fa
    tier 5                          $214798 $2147b6 $2147de $214810
                                    $2148b2 $2148da $214914  -> $215906
    tier 6                          the merge loop $2151de-$2152b8

CORRECTION to the tier table: `$2160f2` and `$216346` are not two
independent functions.  Both are `bsr` targets in their own right, but
`$2160f2` also *falls through* into `$216346` (via `beq $216346` at
`$21623c` and `bra $216346` at `$2162d8`), and `$215dce` branches into
`$2160f2` the same way.  They are entry points into a shared tail, so
they have to be ported as one unit with three doors, not stacked as
tiers.  The closure tool cannot see that -- only reading them does.

The snapshot pair for that unit is already captured:
`pj_0_215f00` -> `pj_1_215f04` (frame 3378, the course-select screen).
Its exit registers are identical to the `span_fill` gate's exit, which
is a nice independent confirmation that the chain really does terminate
in the emitters already ported.

`$216aca` and `$216b50` are blit-queue emitters: they append four-plane
records to the queue at A4 that `road_blitqueue` already consumes, type
7 (masked, minterms `$9fc`/`$930`) and type 6 (unmasked, `$1ff`/`$100`),
with `$3042` supplying a per-plane bit that picks the minterm.  Knowing
these two are the bottom is what makes the rest safe to port upward.

The merge loop at `$2151de` picks the nearest of three iterator streams
(`$2f32`/`$2f34`/`$2f38`), draws it, advances that stream with the
already-ported iterator, and loops — so tier 6 is small once tiers 0-5
land.

### three register traps the memory check cannot see (2026-08-20)

`scen_project` ($2160f2) came out with ZERO memory differences and three
wrong registers.  Every one was a write the port simply did not model:

* **`move.l A4,D0` overwrites D0 completely** -- and the very next
  instruction tests it (`sub.l`, then `cmp.l #$1cac`).  A port that
  writes only the guard condition and not D0 computes the right answer
  and leaves the wrong register.
* **Every `lea` writes its address register.**  `lea (-$4048,A3),A0` is
  not a C constant; it is a store, and the LAST such store before the
  routine returns is what the gate compares.  Table base addresses used
  as literals in the port are the commonest version of this bug.
* **`move.b (A0,D7.w),D7` writes ONE byte of D7**, leaving the upper
  three untouched, and the following `andi.w #$ff,D7` only clears up to
  the word.  A word-sized model of that instruction is wrong in the top
  half.

The lesson generalises: the memory gate proves the routine's effect on
the world, the register gate proves its effect on the machine, and only
the second one catches a port that got the right answer by the wrong
route.  Always declare every register a routine touches via
`stage_expect`, not just the ones that look like return values.

### two tools that change the cost of the rest (2026-08-20)

**`tools/trace_call.py` — port from evidence, not from the opcode.**  The
host already logs 18 longs per instruction (PC, D0-D7, A0-A7, SR) under
`SWIV_STATELOG`, sampled BEFORE the instruction, so consecutive records
differ by exactly what the instruction did.  The tool extracts one call
of a routine and prints the disassembly with the registers each
instruction changed:

    $215098  move.w  ($30d8,A3), D1     d1=00000020
    $21509c  asl.w   #4, D1             d1=00000200

That first line is the `$21508a` trap, settled in one line: the
disassembly cannot tell you whether the game wanted the high or the low
word of that long, and the trace can.  `make statelog` (~11s) then
`make trace PC=21508a`.  It also surfaces the partial-write traps for
free — `move.w ($2ef4,A3),D4` showing `d4=81e00000` says plainly that
the high word is stale and the following `tst.w D4` only tests the low
one.

**`make pcset` — triage by what actually runs.**  A call-graph closure
counts what a subsystem *could* reach; the PC set counts what it *does*.
Over a 9000-frame run into a FOREST race, only **18 of the 33** scenery
functions ever execute, and 7 of those are already ported:

    executed, unported (11):  $215bca $215bd2 $215c32 $215d1e $215dac
                              $215dce $215ea2 $2160f2 $216346 $2169e0
                              $216aca $216b50
    never run (15):           $214798 $2147b6 $2147de $214810 $2148b0
                              $2148b2 $2148da $214914 $21495a $214994
                              $215906 $2159fa $215c60 $2162dc $216310

So the scenery pass needed for a playable FOREST course is **11
routines, not 27**.  The whole `$215906` family and its eight callees are
a second scenery mode selected by `$2e02`/`$2dfe` — almost certainly the
other courses' weather (night, fog, snow) — real code, but not on the
path to the first playable course.  Port the 11, ship a course, come
back for the rest.

CAVEAT: one course, one run.  Before calling any of the 15 dead, re-run
`make pcset` with the other courses reachable.

What is genuinely left, in dependency order:

* **5 scenery pieces** left for a playable FOREST course: `$215dce`,
  `$215ea2`, the thin dispatchers `$215bca/$215bd2` `$215c32` `$215d1e`,
  and the merge loop `$2151de-$2152b8`.  Done so far: `$21508a`
  `$2169e0` `$216aca` `$216b50` `$215dac` `$2160f2` `$216346` `$216812`
  `$216916`.  16 more routines cover the other courses' scenery modes.
* **The scenery / car-sprite / HUD draw passes.**  Until these land the
  drive demo shows a road with no trees, signs, opponents or dashboard,
  and `race_frame_begin` has to stay gated off (rotating the triple
  buffer exposes two frames of stale non-road content).
* **`$210ec4` (176 instructions)** — the routine that owns two of the
  car-position latch copies currently faked by `car_latch_gap()`, the
  one piece of unverified glue in the engine.
* **The sequencer state machine** — phase word `$2fc0(A3)` against the
  table at `$20de12`, park loop `$20df80`.  This is what starts and ends
  a race and runs the frontend; without it the native build can only
  start from a snapshot.
* **Assets and boot.**  Only the FOREST course is extracted
  (`re/pipeline/course_forest.l2c`); course data is expanded at load, so
  the other seven need the loader ported or each course captured.  Track
  tables at chip `$644xx` and fast `$205d88`/`$206188`.
* **Audio** — MOD extraction plus the Imagitec ProTracker replay
  (`$62810`, 9-command dispatcher); no sound in the native build at all.
* **`copper.txt` capture** (WHD_COPPER) for the road copper list.

## Decisions worth flagging

* **0.5 MB ChipMem.**  The SWIV default `CHIP_SIZE = 0x80000` is
  correct for Lotus 2.  The 2 MB override used for Deluxe Galaga is
  *not* applied.
* **No Kickstart.**  `ws_kicksize = ws_kickcrc = 0` per the slave
  header; WHDLoad does not load a Kickstart image.  See
  `issues/known-issues.toml#lotus2-kickstart`.
* **`Disk.1` path.**  SWIV's `disk_load(d0=offset, d1=length, d2=disk,
  a0=dest)` already does what Lotus 2 wants — straight byte copy from
  the cached image.  Disk.1 is the only disk; the host's
  `disk_images[]` slot for disk 1 holds the 958468 bytes.
* **CHIP_SIZE = $80000**, ExpMem region = `$200000..$200000+$900000`
  (the $890000-byte window holds the slave's $882000 request plus
  extra room for the slave stack and resload table).

## Issues filed

See `issues/known-issues.toml`.  None of these are blocking except
`lotus2-rnc2-decoder`, and every one is annotated with `owner`,
`evidence`, `expected_semantics`, and `release_impact`.

## What I did NOT do (deliberate)

* Did not commit the retail slave or `Disk.1`.
* Did not rewrite `amiga.c` / `whdload.c` to be Lotus-2-specific — they
  stay OCS/AGA-neutral copies of the SWIV host.
* Did not invent a kernel family.  SWIV is `salescurve`; Lotus 2 is a
  1991 racing game by a different studio.  Naming one is the wrong move
  until the dispatch tables are in hand.  `lotus2_project.json`
  leaves `kernel.family` as `null` until then.
