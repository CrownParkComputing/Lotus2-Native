# re/pipeline/

Output artefacts from each oracle run land here.  Current content
(2000-frame oracle run, 2026-08-20):

* `oracle_*.ppm` — one screenshot every `--ppm-every` frames.  Frame 1000
  is the title screen (Lotus badge + TURBO CHALLENGE 2), frame 2000 the
  blue Esprit car screen.
* `slave.bin` — the slave's in-memory image ($380000+$2000).
* `fast.bin`, `chip.bin` — full guest RAM dumps at run end
  ($200000+$890000, $0+$80000).
* `pcset.txt` — every distinct PC executed (env-gated by `SWIV_PCSET`).
* `statelog.bin` — 4M per-instruction register records from frame 1500
  (env-gated by `SWIV_STATELOG`, `SWIV_STATELOG_FROM`,
  `SWIV_STATELOG_MAX`).
* `bases.json` — base-detect result: **A3 = $208000** (99.6% dominance).
* `handlers.txt` — dispatch-table result: the Imagitec ProTracker
  replay's 9-command dispatcher at chip `$62810` (the only jump-table
  dispatch; the game kernel is phase-driven direct-call).
* `objlog.txt` — 2000-frame attract-mode base-page timeline (ptrlog).
* `statelog_gameplay.bin`, `fast_gameplay.bin`, `chip_gameplay.bin` —
  register trace (frames 5500+, ~80 frames) and RAM dumps from an
  actual race (--fire-from 2100).  Road interpolator inputs/outputs
  live here.  NOTE: captures must run with video ON; --no-video stalls
  the frame tick (issues/known-issues.toml#lotus2-no-video-stalls-game).
* `combined.bin` + `seeds/` + `disasm/` — seed-disasm input/output
  (Ghidra decomp.c, 192 functions; IRA slave_disasm.txt).
* `objlog.txt` — per-frame kernel object table (not yet captured).
* `copper.txt` — full copper list dump (env-gated by `WHD_COPPER`,
  not yet captured).
* `disasm/slave_disasm.txt` — IRA dump of the slave (seed-disasm stage,
  not yet run).
