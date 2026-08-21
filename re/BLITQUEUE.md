# The blit queues and the interrupt chain that drains them

The game does not blit inline.  It appends *records* to a queue and lets
the blitter-done interrupt walk them: each handler takes the next fields
off A5, programs the blitter, rewrites the level-3 vector at `$6c.w` to
the following handler, and RTEs.  The chain is hand-unrolled, so the
queue has no self-describing headers at all -- the record shape is
whatever the handler that reads it expects.

That is why the grammar below matters: it is the only definition of the
format, and it was read off the handlers rather than inferred from the
producers.

## Producers

| queue | filled by | address |
| --- | --- | --- |
| road | `road_blitqueue()` $2143c2 | `$2f42(A3)`, `$201f40` mid-race |
| scenery / weather | `$216aca`, `$216b50`, `weather_emit()` $214994 | `$200206` mid-race |

`$2f46(A3)` is the READ pointer, swapped with `$2f42` at `$212d76` so a
frame drains the queue the previous frame built.

## The road chain, from $216ff0

Sections are terminated by a negative long where a `BLTDPT` is expected;
the handler `bmi`s past the rest of its section.

```
horizon strip   2 x  [ long BLTDPT, word BLTSIZE ]
$30f0 band      4 x  [ long BLTDPT, word BLTSIZE ]      sentinel at $217026
$30e8 shadow         [ long BLTDPT, long BLTAPT, word BLTCON0, word BLTSIZE ]
                3 x  [ long BLTDPT, long BLTAPT, word BLTSIZE ]
                                                        sentinel at $21709c
$30ee band      4 x  [ long BLTDPT, word BLTSIZE ]      sentinel at $217120
```

Those four shapes are exactly what `road_blitqueue()` writes, which is
the cross-check: producer and consumer agree field for field.

## The scenery / weather chain, from $2171f2

Richer records -- these blits have sources and masks:

```
[ long BLTCON0+BLTCON1, long BLTCMOD+BLTDMOD, long BLTAMOD+BLTBMOD,
  word BLTALWM, long BLTAPT, long BLTCPT, long BLTDPT, word BLTSIZE ]

[ ... same, plus long BLTBPT before BLTCPT ]

[ word BLTCON0, 4 x [ long BLTAPT, long BLTCPT, long BLTDPT,
                      word BLTSIZE ] ]
```

The last of those is the shape `weather_emit()` writes: one word, then
four records of three longs and a size -- one per bitplane, `$20d0`
apart.

## What is not settled yet

Which section drains the weather records is still to be pinned down from
a run rather than by matching shapes: several sections have compatible
prefixes, and the handler actually installed at `$6c.w` at the moment
the weather queue is walked is the evidence that decides it.  Until
that is nailed, a native queue runner would be guessing which registers
each field belongs in.

The runner is what makes the ported weather VISIBLE: `weather_span`,
`weather_emit`, `weather_band` and `weather_step` are all byte-exact
(`make verify-storm`), but they only append to a queue that nothing in
`src/engine/` yet walks.
