# The blit queue

The game does not blit inline.  It appends *records* to a queue and lets
the blitter-done interrupt walk them: each handler takes the record's
fields off A5, programs the blitter, rewrites the level-3 vector at
`$6c.w` to the handler that will run when this blit finishes, and RTEs.
The chain is hand-unrolled, so one blit happens per interrupt and the
frame's drawing is spread across the frame.

## One queue, double-buffered

`$2f42(A3)` is the WRITE pointer and `$2f46(A3)` the READ pointer, swapped
at `$212d76` so a frame drains what the previous frame built.  The two
buffers sit at `$200004` and `$201f40`; a per-instruction trace of a STORM
race shows the same handlers walking both on alternate frames, so this is
one format, not two queues.

## Records are self-describing

`$21718a` is a type dispatcher:

```
    tst.w   (A5)+          ; type word; zero ends the queue
    cmpi.w  #$1,(-$2,A5)   ; ... and a comparison per type
```

Types $1-$7 and $a-$c are handled; anything else falls through.  So a
record is a type word followed by a type-specific body, and the bodies
below are read off the handlers themselves -- they are the only
definition of the format there is.

## The bodies

### type $1  (handler $2171f2, body 28 bytes)

```
long  BLTCON0+BLTCON1
long  BLTCMOD+BLTBMOD
long  BLTAMOD+BLTDMOD
word  BLTALWM
long  BLTAPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
```

### type $2  (handler $217222, body 76 bytes)

```
long  BLTCON0+BLTCON1
long  BLTCMOD+BLTBMOD
long  BLTAMOD+BLTDMOD
word  BLTALWM
long  BLTAPT
long  BLTBPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
word  BLTCON0+BLTCON1
long  BLTAPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
long  BLTAPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
long  BLTAPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
```

### type $3  (handler $2172ba, body 80 bytes)

```
long  BLTCON0+BLTCON1
long  BLTCMOD+BLTBMOD
long  BLTAMOD+BLTDMOD
word  BLTALWM
long  BLTAPT
long  BLTBPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
long  BLTAPT
long  BLTBPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
word  BLTCON0+BLTCON1
long  BLTAPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
long  BLTAPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
```

### type $4  (handler $217356, body 84 bytes)

```
long  BLTCON0+BLTCON1
long  BLTCMOD+BLTBMOD
long  BLTAMOD+BLTDMOD
word  BLTALWM
long  BLTAPT
long  BLTBPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
long  BLTAPT
long  BLTBPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
long  BLTAPT
long  BLTBPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
word  BLTCON0+BLTCON1
long  BLTAPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
```

### type $5  (handler $2173f6, body 86 bytes)

```
long  BLTCON0+BLTCON1
long  BLTCMOD+BLTBMOD
long  BLTAMOD+BLTDMOD
word  BLTALWM
long  BLTAPT
long  BLTBPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
long  BLTAPT
long  BLTBPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
long  BLTAPT
long  BLTBPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
long  BLTAPT
long  BLTBPT
long  BLTCPT
long  BLTDPT
word  BLTSIZE
```

### type $6  (handler $217496, body 34 bytes)

```
word  BLTDMOD
word  BLTCON0+BLTCON1
long  BLTDPT
word  BLTSIZE
word  BLTCON0+BLTCON1
long  BLTDPT
word  BLTSIZE
word  BLTCON0+BLTCON1
long  BLTDPT
word  BLTSIZE
word  BLTCON0+BLTCON1
long  BLTDPT
word  BLTSIZE
```

### type $7  (handler $217518, body 50 bytes)

```
word  $72
word  BLTCON0+BLTCON1
long  BLTDPT
long  BLTAPT
word  BLTSIZE
word  BLTCON0+BLTCON1
long  BLTDPT
long  BLTAPT
word  BLTSIZE
word  BLTCON0+BLTCON1
long  BLTDPT
long  BLTAPT
word  BLTSIZE
word  BLTCON0+BLTCON1
long  BLTDPT
long  BLTAPT
word  BLTSIZE
```

### type $a  (handler $2175b2, body 60 bytes)

```
word  BLTDMOD
word  BLTBMOD
long  BLTDPT
long  BLTBPT
long  BLTAPT
word  BLTSIZE
long  BLTDPT
long  BLTBPT
long  BLTAPT
word  BLTSIZE
long  BLTDPT
long  BLTBPT
long  BLTAPT
word  BLTSIZE
long  BLTDPT
long  BLTBPT
long  BLTAPT
word  BLTSIZE
```

### type $b  (handler $217664, body 60 bytes)

```
word  BLTDMOD
word  BLTBMOD
long  BLTDPT
long  BLTBPT
long  BLTAPT
word  BLTSIZE
long  BLTDPT
long  BLTBPT
long  BLTAPT
word  BLTSIZE
long  BLTDPT
long  BLTBPT
long  BLTAPT
word  BLTSIZE
long  BLTDPT
long  BLTBPT
long  BLTAPT
word  BLTSIZE
```

### type $c  (handler $217716, body 60 bytes)

```
word  BLTDMOD
word  BLTBMOD
long  BLTDPT
long  BLTBPT
long  BLTAPT
word  BLTSIZE
long  BLTDPT
long  BLTBPT
long  BLTAPT
word  BLTSIZE
long  BLTDPT
long  BLTBPT
long  BLTAPT
word  BLTSIZE
long  BLTDPT
long  BLTBPT
long  BLTAPT
word  BLTSIZE
```

## Who writes each type

Searching for the type as a constant finds almost nothing, and that is
the point: **the type is data, not a literal.**  `$216494` writes it
straight out of a variable --

```
$216494  move.w  ($2f4e,A3), (A4)+      ; the type
$216498  cmpi.w  #$4, ($2fbc,A3)
$21649e  addq.w  #1, (-$2,A4)           ; ... one higher when $2fbc >= 4
```

-- so one routine emits types $1 through $5 depending on what
`$2f4e(A3)` holds, which is why grepping for `move.w #$4,(A4)+` turns up
nothing.  Found by tracing A4 instead: for each record boundary, the
instruction that stepped A4 from the record's start to start+2 is the
one that wrote the type.

| type | written by | ported? |
| --- | --- | --- |
| $1, $4, $5 | `$216494`, inside the scenery emitter | yes, `scen_emit()` |
| $6, $7 | `$216acc`, `$216b52` | yes, `span_fill()`'s emitters |
| $b, $c | `$2149c6`, the weather emitter | yes, `weather_emit()` |
| $2, $3, $a | not seen in either captured course | -- |

A single STORM frame's queue holds: one type $1, eight $4, eight $5,
four $b, eight $c, and the zero that ends it.

So every record type this game has been observed to write is produced by
code that is already ported and gated.  What the preview still lacks is
not the emitters -- it is the scheduler above them that decides WHICH
objects to emit.

## Cross-checks

`road_blitqueue()` ($2143c2) writes the shapes types $6 and $7 read, field
for field -- producer and consumer agreeing is what says the extraction
is right rather than plausible.

`weather_emit()` ($214994) writes type **$c** or **$b**: it puts D2 -- $c or
$b -- in the type word, two copies of `D6-2` in BLTDMOD and BLTBMOD, and
then four blocks of `dest, dest, source, size`, one per bitplane, `$20d0`
apart.  62 bytes, and a trace of a STORM frame shows A5 stepping from
`$200206` to `$200244` across exactly one record.  Both destinations are
the same address because BLTDPT and BLTBPT point at the same place: the
blit reads the screen as the B source and writes it back as D.

## What this unblocks

A native queue runner: walk from `$2f46(A3)`, read a type word, program the
engine's blitter model from the body, repeat until the type word is zero.
No interrupts needed -- the interrupt chain exists to spread the work
across a frame on a 7 MHz 68000, not because the order matters.

That is what will make the ported STORM weather visible.  `weather_span`,
`weather_emit`, `weather_band` and `weather_step` are all byte-exact
(`make verify-storm`), and they only append to this queue.

