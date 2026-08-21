# The scenery pass: what is left

`tools/closure.py` walks the call graph below `$21508a` and finds 33
functions, ~1875 instructions.  Nineteen are ported and gated.  Of the
fourteen that are not, **eight never execute** on either course
captured so far (FOREST or STORM), so they are not on the path to
anything visible yet:

```
$214798 $2147b6 $2147de $214810   the $2147xx weather family
$2159fa $215c60 $2162dc $216310
```

## The six that do run

| entry | live instructions at the entry | called from |
| --- | --- | --- |
| `$215bca` | 2 | `$215236` |
| `$215bd2` | 15 | falls through from `$215bca` |
| `$215c32` | 13 | `$215270` |
| `$215d1e` | 26 | `$2152ac` |
| `$215dce` | 16 | `$215c5a`, `$215d60` |
| `$215ea2` | 29 | `$215bf8` |

Those counts are the ENTRIES only, and they are misleading on their own.

## Why they are one unit, not six

`$215ea2` sets up `$2f1a`/`$2f1c`/`$2f1e` from the car's state, picks a
shape out of the `$2438(A3)` table, then branches on `$2fc2(A3)` --
five comparisons, five destinations -- and on FOREST it takes the path
that calls `$2160f2` (ported) and `$215dac` (ported) and then jumps to
`$216126`.

`$215dce` opens with the SAME three moves and ends up in the same place.
`$216126` is a shared tail, and it is where most of the real work is.

So the useful unit to port is not "$215ea2" but "$215ea2, $215dce and
the tail they share", the same shape as `$2160f2`/`$216346` --
"one unit, three doors" -- which `closure.py` cannot see because it
reasons about call edges and these are branches.

## Gate pairs captured

`make scen-snaps` takes entry/exit RAM pairs at the call sites, from a
FOREST race at frame 5600:

```
re/pipeline/scen/e2_  $215bf8 -> $215bfc     $215ea2
re/pipeline/scen/bc_  $215236 -> $21523a     $215bca
re/pipeline/scen/d1_  $2152ac -> $2152b0     $215d1e
```

`$215270` ($215c32) and `$215c5a` ($215dce) do not fire in that window;
they need a frame where the scheduler reaches them, which means finding
one rather than assuming frame 5600 covers everything.

## What this unblocks

Everything below these is already ported and byte-exact: `scen_project`
($2160f2 + $216346), `scen_shape_ptr` ($215dac), `span_fill` ($2169e0)
and its two emitters, and all the blit-queue record types they write
(see BLITQUEUE.md).  The scheduler is the last layer between the ported
engine and scenery appearing in the course preview.
