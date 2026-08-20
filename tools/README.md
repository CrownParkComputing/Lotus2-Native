# tools/

Per-game analysis tools for this title live here.  None of them is wired up
yet (we are still stuck at the RNC2 install-body decoder — see
`../issues/known-issues.toml#lotus2-rnc2-decoder`):

* `kernel_objwalk.py` — walk the kernel task lists and log per-object state
  per frame.  Mirrors `~/BattleSquadron/tools/recomp_studio.py`.
* `find_dispatch.py` — locate the per-frame dispatch table by tracing
  executed PCs in the SWIV-style PC set, group them by call site.
* `objlog-stats.py` — reduce the objlog to activation counts per class.

These will be filled in once the boot gets past the install body and we
have an oracle run that reaches the title screen.
