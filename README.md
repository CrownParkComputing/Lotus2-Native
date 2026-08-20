# Lotus2-Native

Native port of *Lotus Turbo Challenge 2* (CD32 / AGA, 1991, Magnetic Fields /
Gremlin).  Currently at the **ORACLE** stage: a working Musashi-based WHDLoad
host that boots `Lotus2CD32.slave`, decrunches the slave's RNC2 install body,
loads the game from `Disk.1` (122 disk loads, ~546 KB), applies the slave's
patches, and renders non-blank frames.

## Current state (2026-08-20)

| Stage          | Status    | Note                                                       |
| -------------- | --------- | ---------------------------------------------------------- |
| INGEST         | done      | `cp -r /tmp/lotus2/Lotus2CD32/* original/Lotus2CD32/`      |
| ORACLE         | running   | RNC2 fixed; 2000-frame boot: title + car screens, music    |
| base-detect    | done      | A3 = $208000 (99.6%); `re/pipeline/bases.json`             |
| dispatch-table | done      | family = phase-direct; PT-replay cmd table; `handlers.txt` |
| objwalk        | done      | gameplay reached; road interpolator mapped; `objlog.txt`   |
| seed-disasm    | done      | Ghidra: 192 funcs, `disasm/decomp.c`; A3 pinned $208000    |
| TRANSLATE      | running   | 24 routines + 2 composed chains ported, all byte-exact     |
| render-gate    | done      | title AND mid-race frames pixel-exact vs oracle            |
| drive-loop     | done      | `make drive`: emulator-free race frame from a snapshot     |
| PARITY         | future    | needs scheduler $21508a, sprites/HUD, sequencer, audio     |

## Cookbook anchors

* Project root: `/home/jon/Lotus2-Native/`
* Retail binary location: `original/Lotus2CD32/`
* Host source: `src/host/` (copied from SWIV-Amiga; per-cookbook,
  the chipset emulation is title-neutral)
* Musashi: `third_party/musashi/`
* Pipeline artefacts: `re/pipeline/`
* Known issues: `issues/known-issues.toml`
* Stage manifest: `lotus2_project.json`

## Commands

```
make            # build/lotus2 (headless runner) + build/lotus2_native
make boot-test  # 300-frame boot attempt
make selftest   # chipset self-tests (1 known pre-existing video FAIL)
make oracle     # 600-frame capture (--ppm-seq, --ppm-every, --trace)
make gate-capture  # oracle frame+RAM pairs at frames 2000 and 5600
make road-capture  # per-stage RAM snapshots of the racing render chain
make render-gate   # verbs + road stages byte-exact, frames pixel-exact
make clean
```

`SWIV_PCSET=re/pipeline/pcset.txt` before `./build/lotus2` enables the
PC-set dump; `SWIV_STATELOG=re/pipeline/statelog.bin` enables per-
instruction register tracing.

## Retail content

The original WHDLoad install (slave, `Disk.1`, `ReadMe`, `Manual`,
`Message`, `Codes`, plus the `.info` siblings) lives in
`original/Lotus2CD32/`.  This directory is in `.gitignore` and never
committed.  `Disk.1` is a RawDIC dump; it is consumed via the SWIV
host's `disk_load()` path.

## License & legal

The host source, the runner, the RNC2 decoder, and the project manifest
are mine to share.  The retail slave and `Disk.1` are not.  See
`/home/jon/recomp-cookbook/cookbook/RECOMP_COOKBOOK.md` §"Repository and
legal boundary" for the rule we follow.
