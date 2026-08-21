# What is left

Ordered by what unblocks the most.  Everything here is either measured or
says plainly that it is not.

## The port

**1. The scenery scheduler.**  `$215ea2` + `$215dce` + the tail they share
at `$216126`.  This is the last layer between the ported engine and
scenery appearing: everything below it is done and byte-exact --
`scen_project`, `scen_shape_ptr`, `span_fill` and its emitters, and every
blit-queue record type they write.  Gate pairs are captured for three of
the five call sites; `$215270` and `$215c5a` do not fire at frame 5600
and need a frame where the scheduler reaches them.  See SCENERY.md.

**2. The sequencer.**  Phase word `$2fc0(A3)` against the table at
`$20de12`, park loop `$20df80`.  Until this is ported the native engine
can only start from a snapshot; it cannot boot a race itself.  That is
the difference between "the engine is correct" and "the game runs
natively end to end".

**3. `$210ec4`** (176 instructions).  Owns two of the five copies
`car_latch_gap()` currently fakes -- the only unverified glue left in the
engine.

**4. `car_update` / `car_tick` as native overrides.**  Blocked, not
forgotten: their cycle cost is path-dependent (970 in FOREST, 768 in
STORM) and a fixed charge takes the storm gate from 23/25 to 17/25.  Needs
a mechanism, not more porting.

**5. Branch-entered routines** (`$215bca`, `$215bd2`, `$215c32`,
`$215d1e`).  `make override-check` says they are not replaceable because
they are reached by `bcc`/`beq` as well as by call.  Also a mechanism
decision.

**6. STORM's remaining rain difference** in the course gate: one
interrupt frame, A7 off by 6 at `$20f5aa`.  FOUR timing configurations
have been measured and rejected -- the faithful ones were worse.  Do not
retry them; that is the trap.

**7. The eight routines that never execute** on any captured course
(`$2159fa`, `$215c60`, `$2162dc`, `$216310`, and the `$2147xx` family's
siblings).  Not work until a course is found that runs them.  Finding one
is the job, not porting them.

## The remaster

**8. SNOW's weather in the preview.**  Ported and gated (`make
verify-storm`), and deliberately not run: the preview forces `$2ebc` back
to `$60` every frame and SNOW's bands are much larger than STORM's, which
blanks the picture.  Needs understanding, not more code.

**9. Start-line save states.**  The per-course images are captured a few
seconds into a race because that is where a gate needs them, so loading
one puts you already rolling.  A start-line set is another capture, at
the frame the countdown ends.

**10. Sprites and photographs.**  Nothing recovers detail that was never
drawn.  EXPORT on the GRAPHICS page gets the art out; redrawing it is
artwork, and substituting it by address is the easy half.

**11. Opponent car variety.**  Every opponent on screen shares one
palette, so nothing done to a palette or a finished frame can make two of
them differ from each other.  Needs the sprite pass, which is (1).

**12. The vector road.**  Optional.  The geometry is known exactly now --
left edge at `335 - line`, width twice the strip index -- so drawing the
road as vectors at any resolution is a small step.  It would not be
pixel-identical by construction, so it belongs behind a toggle.

## Known, and not bugs in the port

* Something on this desktop asks the window to close about ten seconds
  in.  A capture run ignores it and ends when it has its capture; a
  played session just gets closed.  Not understood.
* `make override-check` exits non-zero by design when a queried routine
  is not replaceable.  Informational, not a failure.
