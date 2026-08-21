# Higher resolution: what is actually possible

Three separate things get called "hi-res", and they need completely
different work.

## 1. Filtering the output — DONE

Scale3x on F3.  Every output pixel is one of the input pixels, so the
palette survives; it rounds the staircase off the art's diagonals.  It
adds no detail, and cannot: the frame it is given is 320 wide.

## 2. The road — NOT what it looked like

The obvious plan was "the road is computed, so compute it bigger".  That
is wrong, and reading `road_bands()` says why.

The road is not rasterised from vectors.  Per screen line the band loop
reads a six-byte record -- position, strip index, band -- and then
**blits a pre-shaded strip**:

```
d0  = f16(a4)              ; strip index
src = f32(a5 + d0*4)       ; a5 = A3+$4e94, a table of strip pointers
shift = (line*2) & $1e     ; barrel shift, the sub-word part of the position
off   = line >> 3          ; and the whole-byte part
bltpt[0] = src + off
bltcon0  = f16($216fb2 + shift)
blitter_run(bl, $95)       ; 21 words x 2 lines
```

So the tarmac, the kerbs and the white markings are ARTWORK -- a set of
pre-drawn strips, one per road width, shifted horizontally by the road's
position on that line.  `$95` is height 2, width 21 words, so one record
covers two screen lines.

That means a genuinely higher-resolution road is one of:

**(a) Sub-pixel strip rendering.**  Keep the game's strips, but draw
them into an N-times buffer: interpolate the road position between
records instead of quantising it to a byte offset plus a 16-step barrel
shift, and scale each strip horizontally.  The road's converging edges
stop stair-casing, which is the thing the eye actually catches.  Still
the game's art, still its colours, and it can be checked against the 1x
path by downsampling.  This is the one I would do.

### The geometry is measurable, and it has been measured

The strips are 1-bit masks and their shape is trivial once you look: a
solid run of 336 pixels, then a GAP, then solid again.  The gap is the
road.  So the width at each distance is not buried in the code -- it is
in the art, and `tools/road_widths.py` reads it out:

```
idx   8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23
px   16 18 20 21 23 24 26 28 32 34 36 38 40 42 44 46
```

Run over every strip, not just the first sixteen, and the table turns
out to be a straight line: **the road's width in pixels is twice the
strip index.**  Index 8 gives 16, index 63 gives 125 -- one short of 126
because the widest strips dither their outer pixel.  There is no width
table to find; the index IS the width.  The wider strips dither their
edges, which is the game getting sub-pixel edges out of one bitplane --
and the centre of that dithered band is the true edge, which is exactly
what a vector renderer wants.

### The per-line record, with real numbers

At A3-$2bd8, stepping six bytes from line $30e4(A3) to $2eaa(A3):

```
line   v      line=$b1-v   index  band
 144  ff8f       290         14   5300
 145  ffa2       271         18   4100
 146  ffb2       255         23   3200
 147  ffb7       250         27   2b00
 148  ffbc       245         31   2500
 149  ffbf       242         36   1f00
 150  ffc1       240         39   1c00
 151  ffc1       240         45   1800
```

The index climbs steadily as the road comes toward the camera -- 14 wide
at the far end of this run, 45 near -- which is the perspective, in one
column of numbers.

Colour does not come from the strips at all.  The band loop writes a
per-line colour RUN LIST through A2, and the copper turns that into the
tarmac, verge and kerb colours.  So the bitplanes carry shape and the
copper carries colour, which is why a vector road can keep the game's
exact palette while drawing its own edges.

**(b) Draw the road from its geometry.**  The per-line data has
everything needed -- centre position, width index (which indexes the
width table at A3-$4048 that `road_perspective` already uses), and the
band colour -- so the road could be drawn as vectors at any resolution
with the markings derived from the course position.  Genuinely sharp at
any size, and NOT pixel-identical to the original by construction.  That
is a remaster of the renderer, not a port of it, and it should be a
choice the player makes rather than something that happens to them.

## 3. The sprites and the photos

Cars, trees, HUD digits, the course-intro pictures: planar art in chip
RAM.  Nothing recovers detail that was never drawn.  EXPORT on the
GRAPHICS page saves them; redrawing them is artwork, and the front end
substituting them by address is the easy half.

## What is still unknown

One thing: how `line` becomes a screen x.  The game splits it into a
byte offset (`line >> 3`) and a sixteen-step barrel shift
(`(line*2) & $1e`), and the destination has its own offsets on top
(`a0 += $41a0`, then `d1*2 * $15`).  The arithmetic does not fall out
cleanly by inspection -- an 8-pixel byte step with a 16-step shift
double-covers -- so it wants measuring against the 1x render rather than
deriving.  That is the last piece before either (a) or (b) can draw a
line in the right place.

## Order

(1) is done.  (2a) next, as a second render path with a scale factor, so
the gated 1x path is untouched and can keep proving the port.  (3) when
there is art.
