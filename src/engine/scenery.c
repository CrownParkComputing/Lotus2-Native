/* scenery.c -- the scenery/object pass below $21508a.
 *
 * The head ($21508a, scen_prepare) and the four iterators live in
 * road.c because they were ported alongside the road chain.  Everything
 * the scheduler DISPATCHES to lives here.
 *
 * These routines are register machines: the game passes spans and
 * indices in D0-D7 and writes its output through A4, the blit-queue
 * pointer that road_blitqueue() already consumes.  Modelling that
 * faithfully means keeping whole 32-bit registers, because the gate
 * compares them: `move.w` leaves the high half alone (so a stale high
 * word survives a whole routine) while `moveq` writes all 32 bits.  A
 * port that keeps only the low words passes the memory check and fails
 * the register one.
 */
#include "engine.h"

#define A3 0x208000u

/* word ops on a 68000 data register: the high half is untouched */
static inline uint16_t w(uint32_t r) { return (uint16_t)r; }
static inline uint32_t setw(uint32_t r, uint16_t v)
{ return (r & 0xffff0000u) | v; }
/* adda.w: the operand is sign-extended and added to the WHOLE register */
static inline uint32_t addaw(uint32_t a, uint16_t v)
{ return a + (uint32_t)(int32_t)(int16_t)v; }

/* $216aca: emit one masked column of the span into the blit queue.
 *
 * Record type 7: mask word, then four planes of (BLTCON0, A source, D
 * dest, BLTSIZE).  $3042 supplies one bit per plane choosing minterm
 * $9fc (A&B into D) or $930; $2f8a is the draw bitmap and $-3d6a(A3)
 * the per-row byte offset table.
 */
static void span_emit_masked(Game *g, Span *s)
{
    uint32_t a2_saved = s->a2;
    pf16(g, s->a4, 7); s->a4 += 2;
    pf16(g, s->a4, w(s->d0)); s->a4 += 2;

    s->d6 = setw(s->d6, (uint16_t)(w(s->d3) + w(s->d3)));
    s->a2 = addaw(s->a2, f16(g, A3 - 0x3d6a
                             + (uint32_t)(int32_t)(int16_t)w(s->d6)));
    s->a2 = addaw(s->a2, w(s->d1));
    s->a2 = addaw(s->a2, w(s->d1));
    s->a2 += f32(g, A3 + 0x2f8a);
    s->a2 += 2;

    s->d6 = setw(s->d6, (uint16_t)((w(s->d7) << 6) + 1));
    s->d1 = setw(s->d1, f16(g, A3 + 0x3042));
    s->d0 = setw(s->d0, 0x20d0);

    for (int plane = 0; plane < 4; plane++) {
        pf16(g, s->a4, 0x09fc); s->a4 += 2;
        int carry = w(s->d1) & 1;
        s->d1 = setw(s->d1, (uint16_t)(w(s->d1) >> 1));
        if (!carry) pf16(g, s->a4 - 2, 0x0930);
        pf32(g, s->a4, s->a2); s->a4 += 4;
        pf32(g, s->a4, s->a2); s->a4 += 4;
        pf16(g, s->a4, w(s->d6)); s->a4 += 2;
        if (plane != 3) s->a2 = addaw(s->a2, w(s->d0));
    }
    s->a2 = a2_saved;
}

/* $216b50: emit the solid middle run.  Record type 6, one A source per
 * plane instead of two and a word count in place of the mask.  D0 is
 * the run length in words. */
static void span_emit_solid(Game *g, Span *s)
{
    uint32_t a2_saved = s->a2;
    pf16(g, s->a4, 6); s->a4 += 2;

    s->d6 = 0x15;                                    /* moveq: all 32 bits */
    s->d6 = setw(s->d6, (uint16_t)(w(s->d6) - w(s->d0)));
    s->d6 = setw(s->d6, (uint16_t)(w(s->d6) + w(s->d6)));
    pf16(g, s->a4, w(s->d6)); s->a4 += 2;

    s->d6 = setw(s->d6, (uint16_t)(w(s->d3) + w(s->d3)));
    s->a2 = addaw(s->a2, f16(g, A3 - 0x3d6a
                             + (uint32_t)(int32_t)(int16_t)w(s->d6)));
    s->a2 = addaw(s->a2, w(s->d1));
    s->a2 = addaw(s->a2, w(s->d1));
    s->a2 += f32(g, A3 + 0x2f8a);
    s->a2 += 2;

    s->d6 = setw(s->d6, (uint16_t)((w(s->d7) << 6) + w(s->d0)));
    s->d1 = setw(s->d1, f16(g, A3 + 0x3042));
    s->d0 = setw(s->d0, 0x20d0);

    for (int plane = 0; plane < 4; plane++) {
        pf16(g, s->a4, 0x01ff); s->a4 += 2;
        int carry = w(s->d1) & 1;
        s->d1 = setw(s->d1, (uint16_t)(w(s->d1) >> 1));
        if (!carry) pf16(g, s->a4 - 2, 0x0100);
        pf32(g, s->a4, s->a2); s->a4 += 4;
        pf16(g, s->a4, w(s->d6)); s->a4 += 2;
        if (plane != 3) s->a2 = addaw(s->a2, w(s->d0));
    }
    s->a2 = a2_saved;
}

/* $2169e0: fill the rectangle D2..D4 x D3..D5 into the blit queue.
 *
 * Clips to the screen and to $2f4a, drops empty spans, then splits by
 * how many 16-pixel columns the span crosses: one column takes a single
 * masked emit with both edge masks ANDed, two take one masked emit per
 * edge, three or more take the two edges plus a solid run between.  The
 * edge mask tables are at $216e60 (left) and $216e40 (right) -- in the
 * code image, which is ExpMem, so they read out of `fast` like any
 * other data.
 */
void span_fill(Game *g, Span *s)
{
    if ((int16_t)w(s->d2) < 0) s->d2 = setw(s->d2, 0);
    if ((int16_t)(w(s->d4) - 0x140) >= 0) s->d4 = setw(s->d4, 0x140);
    if ((int16_t)w(s->d3) < 0) s->d3 = setw(s->d3, 0);
    {
        uint16_t lim = f16(g, A3 + 0x2f4a);
        if ((int16_t)(w(s->d5) - lim) >= 0) s->d5 = setw(s->d5, lim);
    }
    if ((int16_t)(w(s->d2) - w(s->d4)) >= 0) return;   /* empty */
    if ((int16_t)(w(s->d3) - w(s->d5)) >= 0) return;

    s->d7 = setw(s->d7, (uint16_t)(w(s->d5) - w(s->d3)));

    uint16_t cols = (uint16_t)((w(s->d4) >> 4) - (w(s->d2) >> 4));
    s->d1 = setw(s->d1, cols);
    s->d0 = setw(s->d0, (uint16_t)(w(s->d2) >> 4));

    if (cols == 0) {                                   /* $216a24 */
        uint16_t l = (uint16_t)((w(s->d2) & 0xf) * 2);
        uint16_t r = (uint16_t)((w(s->d4) & 0xf) * 2);
        uint16_t m = f16(g, 0x216e60 + l);
        m &= f16(g, 0x216e40 + r);
        s->d0 = setw(s->d0, m);
        s->d1 = setw(s->d1, (uint16_t)(w(s->d2) >> 4));
        span_emit_masked(g, s);
        return;
    }
    if (cols == 1) {                                   /* $216a4e */
        uint16_t l = (uint16_t)((w(s->d2) & 0xf) * 2);
        s->d0 = setw(s->d0, f16(g, 0x216e60 + l));
        s->d1 = setw(s->d1, (uint16_t)(w(s->d2) >> 4));
        span_emit_masked(g, s);
        uint16_t r = (uint16_t)(w(s->d4) & 0xf);
        if (r == 0) return;
        s->d0 = setw(s->d0, f16(g, 0x216e40 + r * 2));
        s->d1 = setw(s->d1, (uint16_t)(w(s->d4) >> 4));
        span_emit_masked(g, s);
        return;
    }
    /* $216a7e: both edges, then the solid middle */
    {
        uint16_t l = (uint16_t)(w(s->d2) & 0xf);
        if (l) {
            s->d0 = setw(s->d0, f16(g, 0x216e60 + l * 2));
            s->d1 = setw(s->d1, (uint16_t)(w(s->d2) >> 4));
            span_emit_masked(g, s);
        }
        uint16_t r = (uint16_t)(w(s->d4) & 0xf);
        if (r) {
            s->d0 = setw(s->d0, f16(g, 0x216e40 + r * 2));
            s->d1 = setw(s->d1, (uint16_t)(w(s->d4) >> 4));
            span_emit_masked(g, s);
        }
        uint16_t first = (uint16_t)((w(s->d2) + 0xf) >> 4);
        s->d0 = setw(s->d0, (uint16_t)((w(s->d4) >> 4) - first));
        s->d1 = setw(s->d1, first);
        span_emit_solid(g, s);
    }
}

/* $215dac: pick the shape pointer for the current weather.
 *
 * $2df8 and $2df6 select between two pointer tables at $2508(A3) and
 * $25c8(A3), indexed by D6.  When BOTH are clear the routine returns
 * with A1 untouched, which is what happens on FOREST -- so on that
 * course this is a no-op and the gate below only covers the early-out.
 * Ported faithfully anyway; the other courses will exercise the rest.
 */
uint32_t scen_shape_ptr(Game *g, uint32_t a1, uint32_t d6)
{
    uint32_t idx = (uint32_t)(int32_t)(int16_t)(uint16_t)d6;
    if (f16(g, A3 + 0x2df8) != 0) return f32(g, A3 + 0x2508 + idx);
    if (f16(g, A3 + 0x2df6) != 0) return f32(g, A3 + 0x25c8 + idx);
    return a1;
}
