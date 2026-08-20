# Porting a Lotus 2 routine to C (route B: semantic rewrite)

You translate 68000 routines from the combined image into C against the
native engine.  Read in this order: `re/ARCH.md` (measured architecture),
`re/VERBS.md` (routine contracts + pseudocode), `src/engine/engine.h`
(the C API), `re/pipeline/disasm/decomp.c` (Ghidra reference — rough on
68k; trust the disassembly, verify against the trace).

## Rules

* One C function per 68000 routine, named after its job
  (`swap_screens`, not `FUN_0020f69e`).  When the original is a park
  loop, translate the loop body and let the engine's frame driver call
  it — do not reproduce the spin.
* The base page is `Game *g`: `move.w $2fe0(a3), d0` → `g->phase`.
  Custom-chip writes via A6 (`move.w d0,$9a(a6)` = INTENA) go through
  the chipset layer; never open-code register addresses in behaviour
  code.
* 68000 flags drive every branch.  Keep the arithmetic widths (w vs l)
  exact — the road interpolator's interpolation signs are the game.
* Tables stay as data: zoom table at fast $204428, stride table
  $20ce94, scanline nibble table $204272, BLTSIZE table $216fb2.
  Reference them by their ARCH.md names; extraction to C arrays happens
  in the assets stage with identity tests against the dumps.
* Verified-vs-contract: VERBS.md marks each routine [verified] (checked
  against `re/pipeline/statelog_gameplay.bin`) or [contract].  Porting
  a [contract] routine means first re-checking it against the trace;
  record the check in the routine's comment.
* No coroutines needed (unlike salescurve titles): routines run to
  completion within a phase.  The sequencer itself becomes a small
  state machine over `g->phase`.

## Verification loop

1. Port the routine.
2. Run the oracle to the same frame window and diff the routine's
   outputs (edge buffers, copper list, palette bank) against the
   gameplay dumps in `re/pipeline/*gameplay.bin`.
3. `make selftest` + `make boot-test` must stay green.
