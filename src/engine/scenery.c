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
#include <stdio.h>
#include <stdlib.h>
#include "engine.h"

#define A3 0x208000u

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
    s->a1 = A3 - 0x3d6a;                             /* lea writes A1 */
    s->a2 = addaw(s->a2, f16(g, s->a1
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
    s->a1 = A3 - 0x3d6a;                             /* lea writes A1 */
    s->a2 = addaw(s->a2, f16(g, s->a1
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
        s->a1 = 0x216e60;
        uint16_t m = f16(g, s->a1 + l);
        s->a1 = 0x216e40;
        m &= f16(g, s->a1 + r);
        s->d0 = setw(s->d0, m);
        s->d1 = setw(s->d1, (uint16_t)(w(s->d2) >> 4));
        span_emit_masked(g, s);
        return;
    }
    if (cols == 1) {                                   /* $216a4e */
        uint16_t l = (uint16_t)((w(s->d2) & 0xf) * 2);
        s->a1 = 0x216e60;
        s->d0 = setw(s->d0, f16(g, s->a1 + l));
        s->d1 = setw(s->d1, (uint16_t)(w(s->d2) >> 4));
        span_emit_masked(g, s);
        uint16_t r = (uint16_t)(w(s->d4) & 0xf);
        s->a1 = 0x216e40;
        if (r == 0) return;
        s->d0 = setw(s->d0, f16(g, s->a1 + r * 2));
        s->d1 = setw(s->d1, (uint16_t)(w(s->d4) >> 4));
        span_emit_masked(g, s);
        return;
    }
    /* $216a7e: both edges, then the solid middle */
    {
        uint16_t l = (uint16_t)(w(s->d2) & 0xf);
        s->a1 = 0x216e60;
        if (l) {
            s->d0 = setw(s->d0, f16(g, s->a1 + l * 2));
            s->d1 = setw(s->d1, (uint16_t)(w(s->d2) >> 4));
            span_emit_masked(g, s);
        }
        uint16_t r = (uint16_t)(w(s->d4) & 0xf);
        s->a1 = 0x216e40;
        if (r) {
            s->d0 = setw(s->d0, f16(g, s->a1 + r * 2));
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

/* ---- $2160f2 / $216346: project a scenery object and queue its blit ----
 *
 * These are two `bsr` targets that also fall into one another
 * ($2160f2 reaches $216346 by branch, and $215dce branches into
 * $2160f2), so they are one routine with several doors rather than a
 * caller and a callee.  scen_project() is the $2160f2 door and ends by
 * calling scen_emit(), the $216346 one.
 *
 * $2160f2 turns a course-relative position into screen coordinates:
 * near objects ($216144) interpolate between two rows of the scanline
 * tables at $-2278(A3) using the sub-row fraction, far ones ($2161bc)
 * take a row straight.  $216346 then reads the object's shape header
 * out of CHIP RAM, clips it, and appends a variable-length record to
 * the blit queue at A4.
 *
 * The shape pointers are chip addresses while the tables are ExpMem, so
 * this is the first ported routine that has to read from both.
 */
static uint8_t m8(const Game *g, uint32_t a)
{ return a < GUEST_CHIP_SIZE ? g->chip[a] : g->fast[a - GUEST_FAST_ADDR]; }

/* muls.w: signed 16x16 into the WHOLE 32-bit destination */
static uint32_t muls_w(uint16_t a, uint16_t b)
{ return (uint32_t)((int32_t)(int16_t)a * (int32_t)(int16_t)b); }
static uint32_t swapw(uint32_t v) { return (v >> 16) | (v << 16); }
static uint16_t ror16(uint16_t v, int n)
{ return (uint16_t)((v >> n) | (v << (16 - n))); }
static int16_t sw(uint32_t r) { return (int16_t)(uint16_t)r; }

/* blit-queue writers */
static void qw(Game *g, Span *s, uint16_t v) { pf16(g, s->a4, v); s->a4 += 2; }
static void ql(Game *g, Span *s, uint32_t v) { pf32(g, s->a4, v); s->a4 += 4; }
/* the two record shapes the variants below are built from */
static void rec_a1(Game *g, Span *s)
{ ql(g,s,s->a0); ql(g,s,s->a1); ql(g,s,s->a2); ql(g,s,s->a2); qw(g,s,w(s->d6)); }
static void rec_no_a1(Game *g, Span *s)
{ ql(g,s,s->a0); ql(g,s,s->a2); ql(g,s,s->a2); qw(g,s,w(s->d6)); }

void scen_project(Game *g, Span *s)
{
    pf16(g, A3 + 0x2ef6, w(s->d4));
    uint16_t cls = f16(g, A3 + 0x2fc2);
    if (cls == 0x00fc || cls == 0x00fd || cls == 0x0078 ||
        cls == 0x0079 || cls == 0x007a) {
        pf16(g, A3 + 0x2fbc, 0xfffe);
        s->d1 = setw(s->d1, 0);
    }

    int far = (w(s->d0) >= 0x40) || (f16(g, A3 + 0x2fbc) == 0xfffe);
    if (!far) {
        pf16(g, A3 + 0x2f1a, (uint16_t)(f16(g, A3 + 0x2f1a) - 1));
        if (w(s->d0) >= f16(g, A3 + 0x2f1a)) return;

        /* $216144: interpolate between two scanline rows */
        s->d2 = setw(s->d2, (uint16_t)(w(s->d2) >> 1));
        s->d3 = setw(s->d3, w(s->d2));
        s->d0 = setw(s->d0, (uint16_t)(w(s->d0) + w(s->d0)));
        s->d4 = setw(s->d4, w(s->d0));
        s->d4 = setw(s->d4, (uint16_t)(w(s->d4) + w(s->d4)));
        s->d4 = setw(s->d4, (uint16_t)(w(s->d4) + w(s->d4)));
        s->a0 = A3 - 0x2278;                         /* lea writes A0 */
        uint32_t t = s->a0 + (uint32_t)(int32_t)sw(s->d4);
        pf16(g, A3 + 0x2f4a, m16(g, t + 4));
        pf16(g, A3 + 0x2ef8, m16(g, t + 6));
        s->d2 = setw(s->d2, m16(g, t + 8));
        s->d5 = setw(s->d5, m16(g, t));
        s->d2 = setw(s->d2, (uint16_t)(w(s->d2) - w(s->d5)));
        s->d2 = swapw(muls_w(w(s->d3), w(s->d2)) * 2);
        s->d2 = setw(s->d2, (uint16_t)(w(s->d2) + w(s->d5)));
        s->d1 = setw(s->d1, (uint16_t)(w(s->d1) - f16(g, A3 + 0x2f1c)));
        s->d1 = setw(s->d1, (uint16_t)(w(s->d1) << 3));
        s->d1 = setw(s->d1, (uint16_t)(w(s->d1) - w(s->d2)));
        s->d2 = setw(s->d2, m16(g, t + 0xa));
        s->d5 = setw(s->d5, m16(g, t + 2));
        s->d2 = setw(s->d2, (uint16_t)(w(s->d2) - w(s->d5)));
        s->d2 = swapw(muls_w(w(s->d3), w(s->d2)) * 2);
        s->d2 = setw(s->d2, (uint16_t)(w(s->d2) + w(s->d5)));
        s->a0 = A3 + 0x4b94;
        uint32_t z = s->a0 + (uint32_t)(int32_t)sw(s->d0);
        pf16(g, A3 + 0x2efe, m16(g, z));
        s->d4 = setw(s->d4, m16(g, z + 2));
        s->d5 = setw(s->d5, m16(g, z));
        s->d4 = setw(s->d4, (uint16_t)(w(s->d4) - w(s->d5)));
        s->d4 = swapw(muls_w(w(s->d3), w(s->d4)) * 2);
        s->d4 = setw(s->d4, (uint16_t)(w(s->d4) + w(s->d5)));
        s->d4 = setw(s->d4, (uint16_t)(w(s->d4) + w(s->d4)));
        s->d1 = swapw(muls_w(w(s->d4), w(s->d1)));
        s->d6 = setw(s->d6, (uint16_t)(f16(g, A3 + 0x2eaa) - 2));
        if (!(w(s->d2) < w(s->d6))) s->d2 = setw(s->d2, w(s->d6));
    } else {
        /* $2161bc: one row straight out of the table */
        if (w(s->d0) >= f16(g, A3 + 0x2f1a)) return;
        s->d0 = setw(s->d0, (uint16_t)(w(s->d0) + w(s->d0)));
        s->d2 = setw(s->d2, w(s->d0));
        s->d2 = setw(s->d2, (uint16_t)(w(s->d2) + w(s->d2)));
        s->d2 = setw(s->d2, (uint16_t)(w(s->d2) + w(s->d2)));
        s->a0 = A3 - 0x2278;
        uint32_t t = s->a0 + (uint32_t)(int32_t)sw(s->d2);
        pf16(g, A3 + 0x2f4a, m16(g, t + 4));
        pf16(g, A3 + 0x2ef8, m16(g, t + 6));
        s->d1 = setw(s->d1, (uint16_t)(w(s->d1) - f16(g, A3 + 0x2f1c)));
        s->d1 = setw(s->d1, (uint16_t)(w(s->d1) << 3));
            s->a0 = A3 - 0x2278;                     /* second lea, same value */
        s->d1 = setw(s->d1, (uint16_t)(w(s->d1) - m16(g, t)));
        s->d2 = setw(s->d2, m16(g, t + 2));
        s->a0 = A3 + 0x4b94;
        uint32_t z = s->a0 + (uint32_t)(int32_t)sw(s->d0);
        s->d4 = setw(s->d4, m16(g, z));
        pf16(g, A3 + 0x2efe, w(s->d4));
        s->d4 = setw(s->d4, (uint16_t)(w(s->d4) + w(s->d4)));
        s->d1 = swapw(muls_w(w(s->d4), w(s->d1)));
        s->d6 = setw(s->d6, (uint16_t)(f16(g, A3 + 0x2eaa) - 2));
        if (!(w(s->d2) < w(s->d6))) s->d2 = setw(s->d2, w(s->d6));
    }

    /* $21620c: common tail */
    if (sw(s->d2) - (int16_t)f16(g, A3 + 0x2f4a) < 0)
        pf16(g, A3 + 0x2f4a, w(s->d2));
    s->d6 = setw(s->d6, f16(g, A3 + 0x2ef6));
    if (w(s->d6) != 0) {
        s->d6 = swapw(muls_w(f16(g, A3 + 0x2efe), w(s->d6)));
        s->d6 = setw(s->d6, (uint16_t)(sw(s->d6) >> 5));
        s->d2 = setw(s->d2, (uint16_t)(w(s->d2) - w(s->d6)));
    }
    if (f16(g, A3 + 0x2fc2) == 0x72) {
        s->d6 = setw(s->d6, (uint16_t)(f16(g, A3 + 0x2efe) >> 5));
        s->d2 = setw(s->d2, (uint16_t)(w(s->d2) - w(s->d6)));
    }

    if (f16(g, A3 + 0x2fbc) == 0xfffe) { scen_emit(g, s); return; }
    if (s->a1 != f32(g, A3 + 0x26a4) && s->a1 != f32(g, A3 + 0x2d1c)) {
        scen_emit(g, s); return;
    }

    /* $21624e: pick the shading row for this object's height band */
    s->d6 = setw(s->d6, w(s->d7));
    s->d7 = setw(s->d7, (uint16_t)(w(s->d7) << 3));
    s->d7 = setw(s->d7, (uint16_t)(w(s->d7) + w(s->d6)));
    s->d6 = setw(s->d6, (uint16_t)(-sw(s->d1)));
    s->d6 = setw(s->d6, (uint16_t)(w(s->d6) + 0x90));
    s->d6 = setw(s->d6, (uint16_t)(sw(s->d6) >> 2));
    uint16_t mode = f16(g, A3 + 0x2fbc);
    int bias = (mode == 3) ||
               (mode == 1 && (f16(g, A3 + 0x304e) != 0 ||
                              f16(g, A3 + 0x3050) != 0));
    if (bias) {
        s->d4 = setw(s->d4, f16(g, A3 + 0x2fbe));
        s->d6 = setw(s->d6, (uint16_t)(w(s->d6) + w(s->d4)));
    }
    s->d4 = setw(s->d4, f16(g, A3 + 0x2ef8));
    s->d4 = setw(s->d4, (uint16_t)(sw(s->d4) >> 2));
    s->d6 = setw(s->d6, (uint16_t)(w(s->d6) - w(s->d4)));
    s->d6 = setw(s->d6, (uint16_t)(sw(s->d6) >> 3));
    if (sw(s->d6) < 0) s->d6 = 0;                    /* moveq: all 32 */
    if (!(sw(s->d6) - 8 < 0)) s->d6 = 8;
    s->d7 = setw(s->d7, (uint16_t)(w(s->d7) + w(s->d6)));
    s->d7 = setw(s->d7, (uint16_t)(w(s->d7) + f16(g, A3 + 0x2fe4)));
    /* move.b writes ONE byte of D7, then andi.w clears the rest */
    s->a0 = 0x216e80;
    s->d7 = (s->d7 & 0xffffff00u) |
            m8(g, s->a0 + (uint32_t)(int32_t)sw(s->d7));
    s->d7 = setw(s->d7, (uint16_t)(w(s->d7) & 0xff));

    if (f16(g, A3 + 0x2df6) != 0 && !(sw(s->d7) - 0x18 < 0)) {
        s->d0 = swapw(f32(g, A3 + 0x2f1e) << 4);
        s->d0 = setw(s->d0, (uint16_t)(w(s->d0) + f16(g, A3 + 0x2fd0)));
        s->d0 = setw(s->d0, (uint16_t)(w(s->d0) >> 4));
        s->d0 = setw(s->d0, (uint16_t)(w(s->d0) << 4));
        s->a0 = A3 - 0x1e78;
        uint32_t r = s->a0 + (uint32_t)(int32_t)sw(s->d0);
        if ((int16_t)m16(g, r + 0xa) < 0) {
            s->a1 = f32(g, A3 + 0x279c);
            s->d7 = setw(s->d7, (uint16_t)(w(s->d7) - 0x18));
        }
    }
    scen_emit(g, s);
}

/* $216346: clip the object's shape and append its blit-queue record.
 *
 * The record layout depends on two selectors: $2fbc (the object class)
 * and, when $2fbc is negative, the long at $2f4c (how many source
 * planes the shape has).  Each variant is the same handful of moves in
 * a different order, walking A1 forward by the plane stride D1 and A2
 * down the bitmap by D4.
 */
/* $216812 -> $216916: the ground-shadow path.
 *
 * Objects whose class $2fbc is $fffe do not blit a shape at all: they
 * lay down three filled spans under the object -- two narrow ones for
 * the shadow's sides and a wide one for its body -- through span_fill.
 * $303c/$303e/$3040 supply the plane mask for each.
 *
 * The $2fc2 >= $f0 branch at $216852 is a different shadow shape that
 * does not run on FOREST; it aborts loudly rather than silently doing
 * nothing.
 */
static void scen_shadow(Game *g, Span *s)
{
    /* $216812: pick the shadow's shading bytes for the current weather */
    uint32_t tab = 0;
    if (f16(g, A3 + 0x2df8) != 0) tab = 0x216bf6;
    else if (f16(g, A3 + 0x2df6) != 0) tab = 0x216bd4;
    if (tab) {
        uint32_t i = (uint32_t)(int32_t)sw(s->d7);
        g->fast[(A3 + 0x303d) - GUEST_FAST_ADDR] = m8(g, tab + i);
        g->fast[(A3 + 0x303f) - GUEST_FAST_ADDR] = m8(g, tab + 0x11 + i);
        g->fast[(A3 + 0x3041) - GUEST_FAST_ADDR] = m8(g, tab + 0x11 + i);
    }

    if (!((int16_t)(f16(g, A3 + 0x2fc2) - 0xf0) < 0)) {
        fprintf(stderr, "scen_shadow: $216856 path ($2fc2 >= $f0) "
                        "not ported\n");
        abort();
    }

    /* $216916 */
    s->d1 = setw(s->d1, (uint16_t)(w(s->d1) + 0xa0));
    pf16(g, A3 + 0x2efa, w(s->d1));
    pf16(g, A3 + 0x2efc, w(s->d2));
    s->d0 = setw(s->d0, (uint16_t)(f16(g, A3 + 0x2efe) >> 4));
    pf16(g, A3 + 0x2f00, w(s->d0));
    pf16(g, A3 + 0x2f00, (uint16_t)(f16(g, A3 + 0x2f00) >> 1));  /* lsr.w (ea) */
    s->d0 = setw(s->d0, (uint16_t)(w(s->d0) + w(s->d0)));
    s->a0 = A3 - 0x4048;
    s->d0 = setw(s->d0, m16(g, s->a0 + (uint32_t)(int32_t)sw(s->d0)));
    pf16(g, A3 + 0x2efe, w(s->d0));

    pf16(g, A3 + 0x3042, f16(g, A3 + 0x303c));
    s->d4 = setw(s->d4, (uint16_t)(f16(g, A3 + 0x2efa) - f16(g, A3 + 0x2efe)));
    s->d5 = setw(s->d5, f16(g, A3 + 0x2efc));
    s->d0 = setw(s->d0, f16(g, A3 + 0x2f00));
    s->d3 = setw(s->d3, (uint16_t)(w(s->d5) - w(s->d0)));
    s->d2 = setw(s->d2, w(s->d4));
    s->d0 = setw(s->d0, (uint16_t)(w(s->d0) >> 3));
    s->d2 = setw(s->d2, (uint16_t)(w(s->d2) - w(s->d0)));
    span_fill(g, s);

    pf16(g, A3 + 0x3042, f16(g, A3 + 0x303c));
    s->d2 = setw(s->d2, (uint16_t)(f16(g, A3 + 0x2efa) + f16(g, A3 + 0x2efe)));
    s->d5 = setw(s->d5, f16(g, A3 + 0x2efc));
    s->d0 = setw(s->d0, f16(g, A3 + 0x2f00));
    s->d3 = setw(s->d3, (uint16_t)(w(s->d5) - w(s->d0)));
    s->d4 = setw(s->d4, w(s->d2));
    s->d0 = setw(s->d0, (uint16_t)(w(s->d0) >> 3));
    s->d4 = setw(s->d4, (uint16_t)(w(s->d4) + w(s->d0)));
    span_fill(g, s);

    pf16(g, A3 + 0x3042, f16(g, A3 + 0x303e));
    if (f16(g, A3 + 0x2fc2) != 0x7a)
        pf16(g, A3 + 0x3042, f16(g, A3 + 0x3040));
    s->d0 = setw(s->d0, (uint16_t)(f16(g, A3 + 0x2f00) >> 3));
    s->d2 = setw(s->d2, (uint16_t)(f16(g, A3 + 0x2efa) - f16(g, A3 + 0x2efe)));
    s->d2 = setw(s->d2, (uint16_t)(w(s->d2) - w(s->d0)));
    s->d4 = setw(s->d4, (uint16_t)(f16(g, A3 + 0x2efa) + f16(g, A3 + 0x2efe)));
    s->d4 = setw(s->d4, (uint16_t)(w(s->d4) + w(s->d0)));
    s->d5 = setw(s->d5, f16(g, A3 + 0x2efc));
    s->d0 = setw(s->d0, f16(g, A3 + 0x2f00));
    s->d5 = setw(s->d5, (uint16_t)(w(s->d5) - w(s->d0)));
    s->d3 = setw(s->d3, w(s->d5));
    s->d0 = setw(s->d0, (uint16_t)(w(s->d0) >> 1));
    s->d3 = setw(s->d3, (uint16_t)(w(s->d3) - w(s->d0)));
    s->d0 = setw(s->d0, (uint16_t)(w(s->d3) + w(s->d5)));
    s->d0 = setw(s->d0, (uint16_t)(w(s->d0) >> 1));
    pf16(g, A3 + 0x2fb4, w(s->d0));
    if (f16(g, A3 + 0x2fc2) == 0xfd) {
        s->d3 = setw(s->d3, (uint16_t)(f16(g, A3 + 0x2f2a) & 0xff));
    }
    span_fill(g, s);
}

void scen_emit(Game *g, Span *s)
{
    if (f16(g, A3 + 0x2fbc) == 0xff00) {
        s->d0 = s->a4 - f32(g, A3 + 0x2f42);   /* move.l A4,D0: all 32 */
        if (s->d0 >= 0x1cac) return;
        pf16(g, A3 + 0x2fbc, 0xffff);
        pf32(g, A3 + 0x2f4c, m32(g, s->a1)); s->a1 += 4;
        pf32(g, A3 + 0x2f50, m32(g, s->a1)); s->a1 += 4;
        s->d7 = setw(s->d7, (uint16_t)(w(s->d7) << 3));
        uint32_t h = s->a1 + (uint32_t)(int32_t)sw(s->d7);
        s->a0 = m32(g, h);
        s->d4 = setw(s->d4, m16(g, h + 4));
        s->d3 = setw(s->d3, m16(g, h + 6));
        s->a0 += s->a1;
        s->d1 = setw(s->d1, (uint16_t)(w(s->d1) + 0xa0));
        s->d7 = setw(s->d7, f16(g, A3 + 0x2fb4));
        s->d0 = setw(s->d0, (uint16_t)(sw(s->d3) >> 1));
        s->d7 = setw(s->d7, (uint16_t)(w(s->d7) + w(s->d0)));
        s->d5 = setw(s->d5, (uint16_t)(w(s->d7) - w(s->d3)));
        s->d7 = setw(s->d7, (uint16_t)(w(s->d7) - 1));
        s->d0 = setw(s->d0, (uint16_t)(sw(s->d4) >> 1));
        s->d1 = setw(s->d1, (uint16_t)(w(s->d1) - w(s->d0)));
    } else {
        s->d0 = s->a4 - f32(g, A3 + 0x2f42);
        if (s->d0 >= 0x1cac) return;
        if (f16(g, A3 + 0x2fbc) == 0xfffe) {
            scen_shadow(g, s);   /* $216812 */
            return;
        }
        s->d0 = s->a1;                         /* move.l A1,D0; beq */
        if (s->d0 == 0) return;
        pf32(g, A3 + 0x2f4c, m32(g, s->a1)); s->a1 += 4;
        pf32(g, A3 + 0x2f50, m32(g, s->a1)); s->a1 += 4;
        s->d7 = setw(s->d7, (uint16_t)(w(s->d7) << 3));
        uint32_t h = s->a1 + (uint32_t)(int32_t)sw(s->d7);
        s->a0 = m32(g, h);
        s->d4 = setw(s->d4, m16(g, h + 4));
        s->d3 = setw(s->d3, m16(g, h + 6));
        s->a0 += s->a1;
        s->d1 = setw(s->d1, (uint16_t)(w(s->d1) + 0xa0));
        s->d7 = setw(s->d7, w(s->d2));
        s->d2 = setw(s->d2, (uint16_t)(w(s->d2) - w(s->d3)));
        s->d2 = setw(s->d2, (uint16_t)(w(s->d2) + 1));
        s->d5 = setw(s->d5, w(s->d2));
        s->d0 = setw(s->d0, (uint16_t)(sw(s->d4) >> 1));
        s->d1 = setw(s->d1, (uint16_t)(w(s->d1) - w(s->d0)));
    }

    /* $2163e2: clip against the bitmap and build the blitter words */
    pf16(g, A3 + 0x2ffe, 0);
    uint16_t frac = (uint16_t)(w(s->d1) & 0xf);
    s->d2 = setw(s->d2, (uint16_t)(w(s->d4) - 1));
    s->d2 = setw(s->d2, (uint16_t)(w(s->d2) & 0xf));
    s->d2 = setw(s->d2, (uint16_t)(w(s->d2) + 1));
    s->d2 = setw(s->d2, (uint16_t)(w(s->d2) + frac));
    s->d2 = setw(s->d2, (uint16_t)(w(s->d2) - 1));
    if (sw(s->d2) - 0x10 < 0) pf16(g, A3 + 0x2ffe, 1);

    pf16(g, A3 + 0x2f04, w(s->d1));
    pf16(g, A3 + 0x2f08, w(s->d5));
    pf16(g, A3 + 0x2f0a, w(s->d7));
    s->d2 = setw(s->d2, (uint16_t)(w(s->d1) + w(s->d4)));
    pf16(g, A3 + 0x2f06, w(s->d2));
    s->d2 = setw(s->d2, (uint16_t)(sw(s->d1) >> 4));
    s->d2 = setw(s->d2, (uint16_t)(w(s->d2) + 1));
    s->d1 = setw(s->d1, (uint16_t)(w(s->d1) & 0xf));
    s->d1 = setw(s->d1, ror16(w(s->d1), 4));
    pf16(g, A3 + 0x303a, w(s->d1));
    s->d4 = setw(s->d4, (uint16_t)(w(s->d4) + 0xf));
    s->d4 = setw(s->d4, (uint16_t)(w(s->d4) >> 4));
    s->d6 = setw(s->d6, (uint16_t)(w(s->d2) + w(s->d4)));
    s->d1 = setw(s->d1, w(s->d4));

    if (sw(s->d2) < 0) {
        if (sw(s->d6) < 0) return;
        s->d0 = setw(s->d0, w(s->d2));
        s->d2 = 0;                                   /* moveq */
        s->d4 = setw(s->d4, (uint16_t)(w(s->d4) + w(s->d0)));
        s->d0 = setw(s->d0, (uint16_t)(w(s->d0) + w(s->d0)));
        s->a0 -= (uint32_t)(int32_t)sw(s->d0);
    }
    if (sw(s->d5) < 0) {
        if (sw(s->d7) < 0) return;
        s->d0 = setw(s->d0, w(s->d5));
        s->d5 = 0;
        s->d3 = setw(s->d3, (uint16_t)(w(s->d3) + w(s->d0)));
        s->d0 = setw(s->d0, (uint16_t)(w(s->d0) + w(s->d0)));
        s->d0 = muls_w(w(s->d1), w(s->d0));
        s->a0 -= (uint32_t)(int32_t)sw(s->d0);
    }
    if (!(sw(s->d6) - 0x15 < 0)) {
        if (sw(s->d2) - 0x15 >= 0) return;
        s->d0 = setw(s->d0, (uint16_t)(w(s->d6) - 0x15));
        s->d4 = setw(s->d4, (uint16_t)(w(s->d4) - w(s->d0)));
        pf16(g, A3 + 0x2ffe, 0);
    }
    if (w(s->d7) > f16(g, A3 + 0x2f4a)) {            /* bls skips */
        s->d7 = setw(s->d7, (uint16_t)(w(s->d7) - f16(g, A3 + 0x2f4a)));
        s->d7 = setw(s->d7, (uint16_t)(w(s->d7) + 1));
        uint16_t before = w(s->d3);
        s->d3 = setw(s->d3, (uint16_t)(w(s->d3) - w(s->d7)));
        if (before <= w(s->d7)) return;              /* sub.w then bls */
    }
    if (w(s->d4) == 0) return;

    qw(g, s, f16(g, A3 + 0x2f4e));
    if (!((int16_t)(f16(g, A3 + 0x2fbc) - 4) < 0))
        pf16(g, s->a4 - 2, (uint16_t)(f16(g, s->a4 - 2) + 1));

    s->d6 = setw(s->d6, (uint16_t)(w(s->d4) + w(s->d4)));
    s->d6 = setw(s->d6, (uint16_t)(w(s->d6) - 0x28));
    s->d6 = setw(s->d6, (uint16_t)(-sw(s->d6)));
    s->d7 = setw(s->d7, (uint16_t)(w(s->d1) - w(s->d4)));
    s->d7 = setw(s->d7, (uint16_t)(w(s->d7) + w(s->d7)));
    s->d7 = setw(s->d7, (uint16_t)(w(s->d7) - 2));
    s->d0 = setw(s->d0, f16(g, A3 + 0x303a));
    s->d1 = setw(s->d1, (uint16_t)(w(s->d0) | 0x0fca));
    qw(g, s, w(s->d1));
    qw(g, s, w(s->d0));
    s->d0 = setw(s->d0, (uint16_t)(w(s->d0) | 0x0b0a));
    if (f16(g, A3 + 0x2ffe) != 0) {
        s->d6 = setw(s->d6, (uint16_t)(w(s->d6) + 2));
        s->d7 = setw(s->d7, (uint16_t)(w(s->d7) + 2));
    }
    qw(g, s, w(s->d6)); qw(g, s, w(s->d7));
    qw(g, s, w(s->d7)); qw(g, s, w(s->d6));
    s->d6 = 0;                                       /* moveq */
    if (f16(g, A3 + 0x2ffe) != 0) s->d6 = 0xffffffffu;
    qw(g, s, w(s->d6));

    s->d1 = f32(g, A3 + 0x2f50);                     /* move.l: all 32 */
    s->a1 = s->a0;
    s->d6 = setw(s->d6, (uint16_t)(f16(g, A3 + 0x2f4e) - 1));
    while (w(s->d6) != 0) {
        s->a0 += s->d1;                              /* adda.l */
        s->d6 = setw(s->d6, (uint16_t)(w(s->d6) - 1));
    }
    s->d2 = setw(s->d2, (uint16_t)(w(s->d2) + w(s->d2)));
    s->d5 = setw(s->d5, (uint16_t)(w(s->d5) + w(s->d5)));
    s->d2 = setw(s->d2, (uint16_t)(w(s->d2) +
                 m16(g, A3 - 0x3d6a + (uint32_t)(int32_t)sw(s->d5))));
    s->a2 = addaw(s->a2, w(s->d2));
    s->a2 += f32(g, A3 + 0x2f8a);
    s->d6 = setw(s->d6, (uint16_t)(w(s->d3) << 6));
    s->d6 = setw(s->d6, (uint16_t)(w(s->d6) + w(s->d4)));
    if (f16(g, A3 + 0x2ffe) == 0)
        s->d6 = setw(s->d6, (uint16_t)(w(s->d6) + 1));
    s->d4 = setw(s->d4, 0x20d0);

    /* the record variants */
    uint32_t D1 = (uint32_t)(int32_t)sw(s->d1);
    uint32_t D4 = (uint32_t)(int32_t)sw(s->d4);
    if ((int16_t)f16(g, A3 + 0x2fbc) < 0) {          /* $21656c */
        uint32_t planes = f32(g, A3 + 0x2f4c);
        if (planes == 1) {                            /* $216608 */
            pf16(g, s->a4 - 0xe, (uint16_t)((f16(g, s->a4 - 0xe) & 0xf000)
                                            | 0x0bfa));
            s->a2 += D4;
            ql(g,s,s->a1); ql(g,s,s->a2); ql(g,s,s->a2); qw(g,s,w(s->d6));
            return;
        }
        if (planes == 2) {                            /* $2165dc */
            rec_a1(g, s);
            s->a2 += D4; qw(g, s, w(s->d0)); rec_no_a1(g, s);
            s->a2 += D4; rec_no_a1(g, s);
            s->a2 += D4; rec_no_a1(g, s);
            return;
        }
        if (planes == 3) {                            /* $216620 */
            rec_a1(g, s);
            s->a1 += D1; s->a2 += D4; rec_a1(g, s);
            s->a2 += D4; qw(g, s, w(s->d0)); rec_no_a1(g, s);
            s->a2 += D4; rec_no_a1(g, s);
            return;
        }
        /* $21658e default */
        rec_a1(g, s);
        s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        if (planes == 5) {                            /* $2165cc */
            s->a1 += D1; s->a2 += D4; rec_a1(g, s);
            return;
        }
        s->a2 += D4; qw(g, s, w(s->d0)); rec_no_a1(g, s);
        return;
    }

    if (f16(g, A3 + 0x2df8) != 0) {                   /* $2167ce */
        uint32_t off = (uint32_t)f16(g, A3 + 0x2fbc);
        off = (uint32_t)(uint16_t)(off - 1) * 0x96fcu;   /* mulu.w */
        s->a0 += off; s->a1 += off;
        rec_a1(g, s);
        for (int i = 0; i < 3; i++) {
            s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        }
        return;
    }

    switch (f16(g, A3 + 0x2fbc)) {
    case 1:                                           /* $216652 */
        s->a0 += s->d1; s->a0 += s->d1;               /* adda.l */
        s->a1 += D1; s->a1 += D1;
        rec_a1(g, s);
        s->a1 -= D1; s->a2 += D4; rec_a1(g, s);
        s->a1 += D1; s->a1 += D1; s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        s->a2 += D4; qw(g, s, w(s->d0)); rec_no_a1(g, s);
        return;
    case 2:                                           /* $216692 */
        s->a0 += s->d1; s->a0 += s->d1;
        rec_a1(g, s);
        s->a1 += D1; s->a1 += D1; s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        s->a2 += D4; qw(g, s, w(s->d0)); rec_no_a1(g, s);
        return;
    case 3:                                           /* $2166ce */
        s->a0 += s->d1; s->a0 += s->d1;
        s->a1 += D1; s->a1 += D1;
        rec_a1(g, s);
        s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        s->a2 += D4; qw(g, s, w(s->d0)); rec_no_a1(g, s);
        return;
    case 5:                                           /* $21670a */
        s->a0 += s->d1; s->a0 += s->d1;
        s->a1 += D1; s->a1 += D1;
        rec_a1(g, s);
        s->a1 -= D1; s->a2 += D4; rec_a1(g, s);
        s->a1 += D1; s->a1 += D1; s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        s->a1 += D1; s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        return;
    case 6:                                           /* $21674e */
        s->a0 += s->d1; s->a0 += s->d1;
        rec_a1(g, s);
        s->a1 += D1; s->a1 += D1; s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        s->a1 += D1; s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        return;
    default:                                          /* $21678e */
        s->a0 += s->d1; s->a0 += s->d1;
        s->a1 += D1; s->a1 += D1;
        rec_a1(g, s);
        s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        s->a1 += D1; s->a1 += D1; s->a2 += D4; rec_a1(g, s);
        return;
    }
}

/* ---- weather ---------------------------------------------------------
 * The rain and snow the courses other than FOREST draw.  `make pcset` on
 * a FOREST race never reaches any of this; a STORM race runs it every
 * frame, through $215906 -> $2148b0/$2148b2/$2148da/$214914 -> here.
 *
 * Both routines are register machines and whole 32-bit registers are
 * carried, because `move.w` leaves the upper half alone and the snapshot
 * gate compares all 32 bits.
 */

/* $21495a-$214992 [snapshot-verified]
 *
 * Works out where a band of weather sits and how wide it is.  D7 indexes
 * the keyframe table at A3-$2278 for the band's screen line; D0 is
 * scaled by $2d0e(A3) -- the scroll phase -- and reduced modulo D3, then
 * rounded up to a whole number of D6-wide steps.  Returns D0 (twice the
 * step count), D1 (the remainder of that step) and D7 (the line).
 */
void weather_span(Game *g, Regs *r)
{
    uint16_t d7 = (uint16_t)(w(r->d[7]) << 3);          /* asl.w #3 */
    r->d[7] = setw(r->d[7], d7);
    uint32_t a0 = A3 - 0x2278;
    r->a[0] = a0;
    d7 = f16(g, a0 + (uint32_t)(int32_t)(int16_t)d7 + 4);
    r->d[7] = setw(r->d[7], d7);

    /* mulu.w writes the WHOLE 32-bit product, not just the low word */
    uint32_t d0 = (uint32_t)w(r->d[0]) * f16(g, A3 + 0x2d0e);
    d0 >>= 8;                                            /* lsr.l #8 */
    uint16_t d3 = w(r->d[3]), d6 = w(r->d[6]);
    if (d3) {
        uint32_t q = d0 / d3, rem = d0 % d3;
        if (q <= 0xffff) d0 = (rem << 16) | (q & 0xffff); /* divu.w */
    }
    d0 = (d0 >> 16) | (d0 << 16);                        /* swap */

    uint16_t d1 = (uint16_t)(d3 - (uint16_t)d0);         /* sub.w D0,D1 */
    d0 = (d0 & 0xffff0000u) | d1;                        /* move.w D1,D0 */

    d0 = (uint32_t)(uint16_t)d0 * 0x2a;                  /* mulu.w #$2a */
    if (d6) {
        uint32_t q = d0 / d6, rem = d0 % d6;
        if (q <= 0xffff) d0 = (rem << 16) | (q & 0xffff);
    }
    d0 = (d0 >> 16) | (d0 << 16);                        /* swap: low=rem */
    d1 = (uint16_t)d0;                                   /* move.w D0,D1 */
    d0 = (d0 >> 16) | (d0 << 16);                        /* swap: low=quot */
    if (d1 != 0) d0 = (d0 & 0xffff0000u) | (uint16_t)(d0 + 1);

    uint32_t d1l = (r->d[1] & 0xffff0000u) | (uint16_t)d0;
    d1l = (uint32_t)(uint16_t)d1l * d6;                  /* mulu.w D6,D1 */
    {
        uint32_t q = d1l / 0x2a, rem = d1l % 0x2a;
        if (q <= 0xffff) d1l = (rem << 16) | (q & 0xffff);
    }
    d1l = (d1l >> 16) | (d1l << 16);                     /* swap */
    d0 = (d0 & 0xffff0000u) | (uint16_t)(w(d0) + w(d0)); /* add.w D0,D0 */

    r->d[0] = d0;
    r->d[1] = d1l;
}

/* $214994-$2149fc [snapshot-verified]
 *
 * Emits the weather into the blit queue -- four records, one per
 * bitplane, $20d0 apart, which is the same queue road_blitqueue()
 * consumes.  The shape pointer comes from the table at $2438(A3), whose
 * entries are CHIP addresses, so it is read range-aware.
 *
 * Returns the queue write pointer (A4) where it left it; nothing is
 * written at all when the band is off the top of the screen or narrower
 * than one step.
 */
uint32_t weather_emit(Game *g, Regs *r)
{
    uint32_t a1 = m32(g, A3 + 0x2438
                      + (uint32_t)(int32_t)(int16_t)w(r->d[5]));
    a1 += (uint32_t)(int32_t)(int16_t)w(r->d[0]);        /* adda.w */
    r->a[1] = a1;

    uint16_t d3 = w(r->d[7]);
    r->a[0] = A3 - 0x3d6a;
    d3 = (uint16_t)(d3 + d3);                            /* add.w D3,D3 */
    d3 = f16(g, (A3 - 0x3d6a) + (uint32_t)(int32_t)(int16_t)d3);
    r->d[3] = (uint32_t)(int32_t)(int16_t)d3;            /* ext.l */
    d3 = (uint16_t)(d3 - w(r->d[4]));                    /* sub.w D4,D3 */
    r->d[3] = (r->d[3] & 0xffff0000u) | d3;
    if ((int16_t)d3 < 0) return r->a[4];                 /* bmi */

    r->d[3] = (uint32_t)(int32_t)(int16_t)d3;            /* ext.l */
    uint16_t d6 = w(r->d[6]);
    if (d6) {
        uint32_t q = r->d[3] / d6, rem = r->d[3] % d6;
        if (q <= 0xffff) r->d[3] = (rem << 16) | (q & 0xffff);
    }
    if (w(r->d[3]) == 0) return r->a[4];                 /* beq on quotient */
    d3 = (uint16_t)(w(r->d[3]) << 6);                    /* asl.w #6 */
    d3 = (uint16_t)(d3 + 1);
    r->d[3] = (r->d[3] & 0xffff0000u) | d3;

    uint32_t a0 = f32(g, A3 + 0x2f8a);
    a0 += (uint32_t)(int32_t)(int16_t)w(r->d[1]);
    a0 += (uint32_t)(int32_t)(int16_t)w(r->d[4]);
    a0 += f32(g, A3 + 0x2ec2);                           /* adda.l */

    uint32_t a4 = r->a[4];
    pf16(g, a4, w(r->d[2])); a4 += 2;
    uint16_t d2 = (uint16_t)(d6 - 2);                    /* move.w D6,D2; subq */
    r->d[2] = (r->d[2] & 0xffff0000u) | d2;
    pf16(g, a4, d2); a4 += 2;
    pf16(g, a4, d2); a4 += 2;
    r->d[2] = 0x20d0;                                    /* move.l #$20d0,D2 */

    /* Four records with the $20d0 step BETWEEN them -- three advances,
     * not four.  Stepping after the last one leaves A0 one plane too far
     * and the gate says so in a single register. */
    for (int plane = 0; plane < 4; plane++) {
        if (plane) a0 += 0x20d0;                         /* adda.w D2,A0 */
        pf32(g, a4, a0); a4 += 4;
        pf32(g, a4, a0); a4 += 4;
        pf32(g, a4, a1); a4 += 4;
        pf16(g, a4, d3); a4 += 2;
    }
    r->a[0] = a0;
    r->a[4] = a4;
    return a4;
}

/* $2148b2 / $2148da / $214914 [snapshot-verified]
 *
 * One band of weather: set up its geometry, then emit it at a list of
 * screen offsets.  The three differ only in those constants, so they are
 * one function and a table -- which is what they are, three copies of
 * the same eight instructions with different immediates.
 *
 * `clr.w D4` and `move.w #x,D4` write the low word only; `moveq` writes
 * all 32 bits.  The gate compares all 32, so the difference is kept.
 */
void weather_band(Game *g, Regs *r, int which)
{
    struct Band {
        uint16_t d0, d3, d7, d6;
        int clr_d7;                       /* clr.w D7 rather than moveq */
        int n;
        struct { uint16_t d4; int moveq_d4; uint32_t d5, d2; } step[6];
    };
    /* 0-2 are the $2148xx family (STORM's rain), 3-6 the $2147xx family
     * (SNOW's falling snow).  Which course runs which was measured, not
     * guessed: `make pcset` on all eight says STORM reaches $2148b2 and
     * SNOW reaches $214798, and nothing else reaches either.
     *
     * The two families differ in more than constants: the snow bands
     * step D5 -- the index into the shape table at $2438(A3) -- for
     * every emit, while the rain bands leave it at zero throughout. */
    static const struct Band BANDS[7] = {
        /* $2148b2 */
        { 0x200, 0x00d0, 0x40, 0x001a, 0, 2,
          { {0x00, 0, 0, 0x0c}, {0x2a, 1, 0, 0x0b} } },
        /* $2148da */
        { 0x400, 0x00cc, 0x20, 0x0044, 0, 4,
          { {0x00, 0, 0, 0x0c}, {0x2a, 1, 0, 0x0c},
            {0x54, 1, 0, 0x0c}, {0x7e, 1, 0, 0x0b} } },
        /* $214914 */
        { 0x600, 0x00dc, 0x00, 0x006e, 1, 6,
          { {0x00, 0, 0, 0x0c}, {0x2a, 1, 0, 0x0c}, {0x54, 1, 0, 0x0c},
            {0x7e, 1, 0, 0x0c}, {0xa8, 0, 0, 0x0b}, {0xd2, 0, 0, 0x0b} } },
        /* $214798 */
        { 0x100, 0x00d0, 0x60, 0x001a, 0, 1,
          { {0x00, 0, 0x00, 0x0a} } },
        /* $2147b6 */
        { 0x200, 0x00cc, 0x40, 0x0044, 0, 2,
          { {0x00, 0, 0x04, 0x0a}, {0x2a, 1, 0x08, 0x0a} } },
        /* $2147de */
        { 0x400, 0x00dc, 0x20, 0x006e, 0, 3,
          { {0x00, 0, 0x0c, 0x0a}, {0x2a, 1, 0x10, 0x0a},
            {0x54, 1, 0x14, 0x0a} } },
        /* $214810 */
        { 0x800, 0x0130, 0x00, 0x0098, 1, 4,
          { {0x00, 0, 0x18, 0x0a}, {0x2a, 1, 0x1c, 0x0a},
            {0x54, 1, 0x20, 0x0a}, {0x7e, 1, 0x24, 0x0a} } },
    };
    if (which < 0 || which > 6) return;
    const struct Band *b = &BANDS[which];

    r->d[0] = setw(r->d[0], b->d0);          /* move.w */
    r->d[3] = setw(r->d[3], b->d3);
    if (b->clr_d7) r->d[7] = setw(r->d[7], 0);    /* clr.w D7 */
    else           r->d[7] = b->d7;               /* moveq */
    r->d[6] = setw(r->d[6], b->d6);
    weather_span(g, r);

    for (int i = 0; i < b->n; i++) {
        if (b->step[i].moveq_d4) r->d[4] = b->step[i].d4;   /* moveq */
        else r->d[4] = setw(r->d[4], b->step[i].d4);        /* clr.w/move.w */
        r->d[5] = b->step[i].d5;                            /* moveq #n,D5 */
        r->d[2] = b->step[i].d2;                            /* moveq */
        weather_emit(g, r);
    }
}

/* $215906-$2159ea [snapshot-verified]
 *
 * One step of the weather state machine.  $2ebc(A3) walks $60 -> $40 ->
 * $20 -> $0 -> $ffe0, drawing a different band at each stop, and
 * $2e02(A3) picks which family of bands: the $2147xx set or the $2148xx
 * set.  A0-A2 are saved around every call, so the caller's pointers
 * survive.
 *
 * BOTH families are ported.  `make pcset` over all eight courses says
 * STORM is the only one that reaches the $2148xx set and SNOW the only
 * one that reaches the $2147xx set; the other six draw no weather at
 * all through this pass.
 */
void weather_step(Game *g, Regs *r)
{
    uint16_t phase = f16(g, A3 + 0x2ebc);
    int family_b = f16(g, A3 + 0x2e02) != 0;
    int band = phase == 0x60 ? 0 : phase == 0x40 ? 1
             : phase == 0x20 ? 2 : phase == 0x00 ? 3 : -1;
    if (band < 0) return;

    uint32_t a0 = r->a[0], a1 = r->a[1], a2 = r->a[2];
    if (family_b) {
        /* $2148b0 for phase $60 is a bare rts: the first stop draws
         * nothing on the rain family. */
        if (band >= 1) weather_band(g, r, band - 1);
    } else {
        weather_band(g, r, 3 + band);       /* the snow family, all four */
    }
    r->a[0] = a0; r->a[1] = a1; r->a[2] = a2;   /* movem.l (A7)+,A0-A2 */

    static const uint16_t NEXT[4] = {0x40, 0x20, 0x00, 0xffe0};
    pf16(g, A3 + 0x2ebc, NEXT[band]);
}

/* $2159ec: run the state machine until $2ebc goes negative. */
void weather_pass(Game *g, Regs *r)
{
    for (int guard = 0; guard < 8; guard++) {
        if ((int16_t)f16(g, A3 + 0x2ebc) < 0) return;
        weather_step(g, r);
    }
}
