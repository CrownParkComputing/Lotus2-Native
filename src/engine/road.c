/* road.c -- native ports of the Lotus 2 racing render pipeline (the BSR
 * chain at $212f12; see re/pipeline/disasm/road_213534.txt).
 *
 * Verification: each stage is checked byte-for-byte against entry/exit RAM
 * snapshots (SWIV_SNAP_PCS on the host, re/pipeline/road/st_*), so every
 * port here is a proven input->output transform, not a reading of the
 * disassembly.  68000 width semantics are kept exact: word ops touch only
 * the low 16 bits, and the interpolator's fixed-point accumulate is the
 * original swap/add.l/swap dance.
 */
#include <stdint.h>
#include "engine.h"
#include "blitter.h"

#define A3 0x208000u

/* $214268 (which=0) / $21427a (which=1), exits $214342 [snapshot-verified]
 *
 * Road edge interpolator: walks the per-segment keyframe records backwards
 * (8-byte records ending at A3-$2278 + count*8), interpolates the edge
 * X between keyframes with the slope tables at A3-$3bd8 ($204428), and
 * emits one (colour, x, scale) word-triple per road line into the stream
 * at A3-$2bd8/-$2728 that the band blitter ($213534) consumes.  D7 is a
 * line counter in $100 fixed-point steps, stepped by the zoom word it
 * stores at $2ff4(A3). */
void road_interpolate(Game *g, int which)
{
    uint32_t a4, a1, a0;
    uint16_t d0, d1, d2, d5, d7;
    uint32_t d0l, d3l;

    if (!which) {
        a4 = A3 - 0x2278;                    /* $205d88: keyframe records */
        a1 = A3 - 0x2bd8;                    /* $205428: output stream */
        d0 = f16(g, A3 + 0x30ce);            /* record count */
    } else {
        a4 = A3 - 0x2278;
        a1 = A3 - 0x2728;                    /* $2058d8: second stream */
        d0 = f16(g, A3 + 0x31a2);
    }

    uint16_t d6 = (uint16_t)(d0 * 2);
    d7 = (uint16_t)((d0 - 1) << 8);
    a0 = A3 + 0x4b94 + d6;                   /* $20cb94 + count*2 */
    d6 = (uint16_t)(d6 * 4);
    a4 += d6;                                /* records end (walk down) */

    /* $2142a0 */
    const uint32_t a2 = A3 - 0x3bd8;         /* $204428 slope/zoom tables */
    a4 -= 4;
    a4 -= 2; d1 = f16(g, a4);
    a4 -= 6;
    a0 -= 2; d0 = f16(g, a0);
    a1 += (uint16_t)(d1 * 6);                /* position at first line*6 */
    d0l = d0;

    for (;;) {                               /* $2142b8 */
        a4 -= 2; d5 = f16(g, a4);
        a4 -= 2; d2 = f16(g, a4);
        a4 -= 4;
        a0 -= 2; uint16_t d4 = f16(g, a0);

        if (d1 == d5) {                      /* $2142c0 */
            d0l = (d0l & 0xffff0000u) | d4;
        } else {
            d1 = (uint16_t)(d1 - d5);
            if (d1 == 0xffff) {              /* cmp.w #-1: one-line gap */
                pf16(g, a1, d2); a1 += 2;    /* $214330 */
                pf16(g, a1, (uint16_t)d0l); a1 += 2;
                pf16(g, a1, d7); a1 += 2;
                d0l = (d0l & 0xffff0000u) | d4;
                d1 = d5;
            } else if (!(d1 & 0x8000)) {     /* positive: rewind stream */
                a1 -= (uint16_t)(d1 * 2);    /* $2142d0 */
                a1 -= (uint16_t)(d1 * 4);
                d0l = (d0l & 0xffff0000u) | d4;
                d1 = d5;
            } else {                         /* $2142de: interpolate */
                d1 = (uint16_t)-d1;
                d0l = (uint32_t)(int32_t)(int16_t)(uint16_t)d0l; /* ext.l */
                uint16_t d3 = (uint16_t)(d1 * 4);
                d3l = f32(g, a2 + 0x40 + d3);
                d3l <<= 4;
                d3l = (d3l >> 16) | (d3l << 16);        /* swap */
                pf16(g, A3 + 0x2ff4, (uint16_t)d3l);    /* zoom step */
                d3 = (uint16_t)(((uint16_t)(d4 - (uint16_t)d0l) & 0xfff0)
                                + d1);
                d3 = (uint16_t)(d3 * 4);
                d3l = f32(g, a2 + (int16_t)d3);
                d1 = (uint16_t)(d1 - 2);
                do {                          /* $214308 */
                    pf16(g, a1, d2); a1 += 2;
                    pf16(g, a1, (uint16_t)d0l); a1 += 2;
                    pf16(g, a1, d7); a1 += 2;
                    d7 = (uint16_t)(d7 - f16(g, A3 + 0x2ff4));
                    d0l = (d0l >> 16) | (d0l << 16);    /* swap */
                    d0l += d3l;                          /* add.l */
                    d0l = (d0l >> 16) | (d0l << 16);    /* swap */
                } while (d1-- != 0);          /* dbra */
                pf16(g, a1, d2); a1 += 2;     /* $21431c */
                pf16(g, a1, (uint16_t)d0l); a1 += 2;
                pf16(g, a1, d7); a1 += 2;
                d7 = (uint16_t)(d7 - f16(g, A3 + 0x2ff4));
                d7 = (uint16_t)((d7 & 0xff00) + 0x100);
                d0l = (d0l & 0xffff0000u) | d4;
                d1 = d5;
            }
        }
        d7 = (uint16_t)(d7 - 0x100);          /* $21433a */
        if (d7 & 0x8000) break;               /* bpl */
    }
    (void)d0;
}

/* $2143c2-$2144e0 [snapshot-verified]
 *
 * Blit-queue descriptor builder: appends the frame's road/scenery blit
 * records to the queue at A4 ($2f42(A3), $201f40 mid-race).  Four record
 * shapes, each terminated by a $ffffffff sentinel when its enable word is
 * zero: horizon strip (2 records), the $30f0 band (4), the $30e8 shadow
 * band (4 wide records with source AND destination longs), and the $30ee
 * band (4).  D7 = $20d0 is the fixed inter-record destination stride. */
void road_blitqueue(Game *g)
{
    uint32_t a4 = f32(g, A3 + 0x2f42);
    const uint32_t d7 = 0x20d0;
    uint32_t d6 = f32(g, A3 + 0x2f8a);
    uint32_t a0 = d6;
    uint16_t d1 = f16(g, A3 + 0x30e4);
    uint16_t d0 = (uint16_t)(d1 * 0x2a);
    a0 += (uint32_t)(int32_t)(int16_t)d0;            /* adda.w */
    a0 += 2;
    d0 = f16(g, A3 + 0x2eaa);
    d0 = (uint16_t)(d0 - d1);
    d0 = (uint16_t)((d0 << 6) + 0x14);
    pf32(g, a4, a0); a4 += 4;
    pf16(g, a4, d0); a4 += 2;
    a0 += (uint32_t)(int32_t)(int16_t)d7;
    pf32(g, a4, a0); a4 += 4;
    pf16(g, a4, d0); a4 += 2;

    d0 = f16(g, A3 + 0x30f0);                        /* $2143f4 */
    if (d0) {
        d0 = (uint16_t)((d0 << 6) + 0x14);
        a0 = d6 + 2;
        for (int i = 0; i < 4; i++) {
            pf32(g, a4, a0); a4 += 4;
            pf16(g, a4, d0); a4 += 2;
            a0 += (uint32_t)(int32_t)(int16_t)d7;
        }
    } else {
        pf32(g, a4, 0xffffffffu); a4 += 4;           /* $21441c */
    }

    d0 = f16(g, A3 + 0x30e8);                        /* $214422 */
    if (d0) {
        uint16_t d5 = (uint16_t)((d0 << 6) + 0x15);
        uint32_t d3 = f32(g, A3 + 0x2d28);
        uint32_t d1l = f16(g, A3 + 0x30e2) & 0xfff;
        d1l *= 0x2800;                               /* mulu.w -> long */
        d1l = (d1l >> 16) | (d1l << 16);             /* swap */
        d0 = (uint16_t)d1l;
        uint16_t t = (uint16_t)(((uint16_t)d1l >> 4) * 2);
        d3 += (uint32_t)(int32_t)(int16_t)t;         /* ext.l; add.l */
        uint32_t d4 = (uint32_t)f16(g, A3 + 0x30ea) * 0x78;
        d3 += d4;
        d0 = (uint16_t)(((d0 & 0xf) ^ 0xf) << 4);
        d0 = (uint16_t)((d0 << 8) | 0x9f0);
        uint16_t d1w = d0;
        uint32_t d0l = (uint32_t)f16(g, A3 + 0x30e6) * 0x2a;
        d0l += d6;
        d4 = (uint32_t)f16(g, A3 + 0x2c8e) * 0x78;
        pf32(g, a4, d0l); a4 += 4;                   /* $21447a */
        pf32(g, a4, d3); a4 += 4;
        pf16(g, a4, d1w); a4 += 2;
        pf16(g, a4, d5); a4 += 2;
        for (int i = 0; i < 3; i++) {
            d3 += d4;
            d0l += d7;
            pf32(g, a4, d0l); a4 += 4;
            pf32(g, a4, d3); a4 += 4;
            pf16(g, a4, d5); a4 += 2;
        }
    } else {
        pf32(g, a4, 0xffffffffu); a4 += 4;           /* $2144a2 */
    }

    d0 = f16(g, A3 + 0x30ee);                        /* $2144a8; bls == beq */
    if (d0) {
        d0 = (uint16_t)((d0 << 6) + 0x14);
        a0 = d6 + 2;
        uint16_t d1b = (uint16_t)(f16(g, A3 + 0x30ec) * 0x2a);
        a0 += (uint32_t)(int32_t)(int16_t)d1b;
        for (int i = 0; i < 4; i++) {
            pf32(g, a4, a0); a4 += 4;
            pf16(g, a4, d0); a4 += 2;
            a0 += (uint32_t)(int32_t)(int16_t)d7;
        }
    } else {
        pf32(g, a4, 0xffffffffu); a4 += 4;           /* $2144da */
    }
}

/* $214354-$2143c0 [snapshot-verified]
 *
 * Band bounds for one road view.  A4 is the view's parameter block
 * (A3+$3054 for the near view via $214344, A3+$3128 for the far one via
 * $21434c).  It takes the band height at +$8c and the clip limit at +$90,
 * works out where the band lands against the screen height ($2eaa) minus
 * the fixed $38 inset and the horizon offset ($2c8e), clamps both edges
 * into 0..limit, and writes back the clipped top/bottom, the amount cut
 * off the top, and the two spans the blitter needs.
 */
void road_band_bounds(Game *g, uint32_t view)
{
    uint16_t d0 = f16(g, view + 0x8c);
    pf16(g, view + 0xa6, d0);
    uint16_t d1 = f16(g, view + 0x90);
    d0 = (uint16_t)((int16_t)d0 >> 1);             /* asr.w #1 */

    uint16_t d7 = f16(g, A3 + 0x2eaa);
    d7 = (uint16_t)(d7 - 0x38);
    d7 = (uint16_t)(d7 - f16(g, A3 + 0x2c8e));
    d7 = (uint16_t)(d7 - d0);
    uint16_t d2 = d7;
    d7 = (uint16_t)(d7 + f16(g, A3 + 0x2c8e));
    uint16_t d3 = d7;
    uint16_t d4 = 0;

    if ((int16_t)d2 < 0) {                          /* $21437a */
        d0 = (uint16_t)-d2;
        d2 = 0;
        d4 = d0;
    }
    if ((int16_t)d3 < 0) d3 = 0;                    /* $214384 */
    if ((int16_t)(d1 - d3) < 0) d3 = d1;            /* $21438a */
    if ((int16_t)(d1 - d2) < 0) d2 = d1;            /* $214392 */

    uint16_t d5 = (uint16_t)(d1 - d3);
    uint16_t d6 = (uint16_t)(d3 - d2);
    d7 = d2;

    pf16(g, view + 0x92, d2);
    pf16(g, view + 0x98, d3);
    pf16(g, view + 0x96, d4);
    pf16(g, view + 0x9a, d5);
    pf16(g, view + 0x94, d6);
    pf16(g, view + 0x9c, d7);
}

/* $213edc-$214266 [snapshot-verified via $213eb4]
 *
 * Keyframe generator.  Walks the course records forward from the car's
 * segment, accumulating curvature into a lateral position (D0 -> D2) and
 * the vertical delta into a height (D1 -> D3), and emits one 8-byte
 * keyframe per step into the buffer the interpolator later walks
 * backwards: {lateral, screen line, running minimum line, curvature}.
 *
 * The original is unrolled into 18 copies of the same block: one counted
 * by $2dca(A3) (the part-group the car is partway through), then sixteen
 * straight-line copies, then one counted by $2dcc(A3).  Only the two
 * counted groups repeat the block without reloading the record, which is
 * why the loop below reloads on every group except those repeats.
 *
 * A4 walks the zoom table at A3+$4a94 two bytes per keyframe, reading its
 * scale at +$100 and its offset at the walking pointer.  The generator
 * stops early the moment a keyframe lands at or past the screen bottom
 * ($2eaa) -- that is the horizon, and the tail then writes the terminator
 * the interpolator expects.
 *
 * Returns the keyframe count (D0); *out_line receives D1 (the horizon
 * line), matching what $213eb4 stores at $30ce/$30e4.
 */
uint16_t road_keyframes(Game *g, uint32_t a2, uint32_t course_pos,
                        uint16_t *out_line)
{
    const uint32_t a1 = a2;                       /* movea.l a2,a1 */
    uint32_t a4 = A3 + 0x4a94;
    uint16_t low = (uint16_t)course_pos;
    uint16_t d1 = (uint16_t)((low >> 12) & 0xf);  /* rol.w #4; andi #$f */
    pf16(g, A3 + 0x2dca, (uint16_t)(0xf - d1));
    pf16(g, A3 + 0x2dcc, (uint16_t)(d1 + 1));

    uint16_t hi = (uint16_t)(course_pos >> 16);   /* swap */
    uint32_t a0 = (A3 - 0x1e7d)
                + (uint32_t)(int32_t)(int16_t)(uint16_t)(hi << 4);

    uint16_t d0 = 0, d2 = 0, d3 = 0, d6 = 0;
    d1 = 0;
    uint16_t d7 = f16(g, A3 + 0x2eaa);
    uint16_t d4 = 0, d5 = 0;
    int stopped = 0;

#define KF_LOAD()                                                          \
    do {                                                                   \
        a0 += 0x10;                                                        \
        d4 = (uint16_t)(int16_t)(int8_t)                                   \
             g->fast[a0 - 0xb - GUEST_FAST_ADDR];                          \
        d5 = (uint16_t)(int16_t)(int8_t)                                   \
             g->fast[a0 - 0xa - GUEST_FAST_ADDR];                          \
        d5 = (uint16_t)(d5 + d5);                                          \
    } while (0)

/* one emitted keyframe; sets `stopped` at the horizon ($21423a exit) */
#define KF_BLOCK()                                                         \
    do {                                                                   \
        d0 = (uint16_t)(d0 + d4);                                          \
        d1 = (uint16_t)(d1 - d5);                                          \
        d2 = (uint16_t)(d2 + d0);                                          \
        d3 = (uint16_t)(d3 + d1);                                          \
        pf16(g, a2, d2); a2 += 2;                                          \
        int32_t prod = (int32_t)(int16_t)d3                                \
                     * (int16_t)f16(g, a4 + 0x100);                        \
        uint32_t sw = ((uint32_t)prod >> 16) | ((uint32_t)prod << 16);     \
        d6 = (uint16_t)((uint16_t)sw + f16(g, a4));                        \
        a4 += 2;                                                           \
        pf16(g, a2, d6); a2 += 2;                                          \
        if (d6 >= f16(g, A3 + 0x2eaa)) { stopped = 1; break; }             \
        if ((int16_t)(d6 - d7) < 0)                                        \
            d7 = (uint16_t)((d7 & 0xff00) | (d6 & 0xff));                  \
        pf16(g, a2, d7); a2 += 2;                                          \
        pf16(g, a2, d0); a2 += 2;                                          \
    } while (0)

    /* group A: pre-tested loop on $2dca -- the part-group the car is
     * already partway through (tst.w/beq at $213f22, so a zero count
     * skips it entirely) */
    KF_LOAD();
    if (f16(g, A3 + 0x2dca) != 0) {
        for (;;) {
            KF_BLOCK();
            if (stopped) break;
            uint16_t left = (uint16_t)(f16(g, A3 + 0x2dca) - 1);
            pf16(g, A3 + 0x2dca, left);
            if (!left) break;
        }
    }

    /* The sixteen straight-line blocks are a loop body: $213f56 sets
     * $2f9a = 7 and $2141ec decrements it, jumping back to $213f5c -- so
     * it is 7 x 16 keyframes.  Crucially the record load sits at $213f5c,
     * INSIDE that jump target but OUTSIDE the sixteen blocks, so all
     * sixteen share one curvature/slope pair and the course advances once
     * per group of sixteen, not once per keyframe. */
    if (!stopped) {
        pf16(g, A3 + 0x2f9a, 7);
        for (;;) {
            KF_LOAD();
            for (int k = 0; k < 16 && !stopped; k++)
                KF_BLOCK();
            if (stopped) break;
            uint16_t left = (uint16_t)(f16(g, A3 + 0x2f9a) - 1);
            pf16(g, A3 + 0x2f9a, left);
            if (!left) break;
        }
    }

    /* final group: do-while on $2dcc ($214206, no pre-test) */
    if (!stopped) {
        KF_LOAD();
        for (;;) {
            KF_BLOCK();
            if (stopped) break;
            uint16_t left = (uint16_t)(f16(g, A3 + 0x2dcc) - 1);
            pf16(g, A3 + 0x2dcc, left);
            if (!left) break;
        }
    }
#undef KF_LOAD
#undef KF_BLOCK

    uint16_t ret_d0, ret_d1;
    if (stopped) {                                /* $21423a */
        if ((int16_t)(d6 - f16(g, A3 + 0x2eaa)) >= 0) {   /* bpl $214252 */
            ret_d1 = d7;
            ret_d0 = (uint16_t)(((uint16_t)(a2 - a1)) >> 3);
        } else {
            pf16(g, a2, 0);
            pf16(g, a2 - 2, 0);
            ret_d1 = 0;
            ret_d0 = (uint16_t)((((uint16_t)(a2 - a1)) >> 3) + 1);
        }
    } else {                                      /* $214232 */
        ret_d1 = d7;
        ret_d0 = 0x80;
    }

    uint32_t p = a1;                              /* $21425a terminator */
    pf16(g, p, 0); p += 2;
    pf16(g, p, f16(g, A3 + 0x2eaa)); p += 2;
    pf16(g, p, f16(g, A3 + 0x2eaa)); p += 2;
    pf16(g, p, 0);

    if (out_line) *out_line = ret_d1;
    return ret_d0;
}

/* $213eb4-$213ec6 [snapshot-verified]: run the generator for the near view. */
void road_keyframes_near(Game *g)
{
    uint16_t line = 0;
    uint16_t count = road_keyframes(g, A3 - 0x2278, f32(g, A3 + 0x30d8),
                                    &line);
    pf16(g, A3 + 0x30ce, count);
    pf16(g, A3 + 0x30e4, line);
}

/* $213416-$213532 [snapshot-verified via $21337c]
 *
 * Perspective pass over the edge stream.  Walks the (colour, x, z) triples
 * the interpolator produced, starting at the horizon line, and rewrites
 * each one in place: x becomes x>>4 (the screen column), and the colour
 * word becomes the perspective-scaled edge, computed as
 * -(z + D4) * (x*2) >> 16 minus the width table entry at A3-$4048
 * indexed by (x>>4)*2.
 *
 * Four variants share that body.  Which one runs depends on the steering
 * word D0 and the guard at $2dee(A3): with no guard, or D0 == 0, the
 * plain pass runs and returns $ff; D0 == 2 takes the centred variant;
 * otherwise a left ($213442) or right ($213482) variant also tracks the
 * extreme edge, returning it in D3 and the corresponding colour in D2.
 *
 * Returns D3 (the extreme) and writes D2 through *out_colour, matching
 * what $21337c stores at $2eb4/$2eb8.
 */
static uint16_t road_perspective(Game *g, uint32_t a4, uint16_t d4,
                                 uint16_t d1_in, uint16_t d0_in,
                                 uint16_t d3_in, uint16_t *out_colour)
{
    const uint32_t a1 = A3 - 0x4048;
    uint16_t d7 = d1_in;
    a4 += (uint16_t)(d1_in * 6);              /* three adda.w of d1*2 */
    uint16_t d2 = 0x7fff;
    d4 = (uint16_t)(d4 << 3);
    /* D3 is INHERITED from the caller: the plain path never initialises
     * it, so the extreme this returns is whatever the previous stage left
     * in the register.  The verifier feeds it from the snapshot regs. */
    uint16_t d3 = d3_in;

    enum { PLAIN, LEFT, RIGHT, CENTRED } mode;
    if (f16(g, A3 + 0x2dee) == 0) mode = PLAIN;
    else if (d0_in == 2) mode = CENTRED;
    else if (d0_in == 0) mode = PLAIN;
    else if ((int16_t)d0_in > 0) mode = RIGHT;
    else mode = LEFT;

    if (mode == LEFT) d3 = 0x1000;              /* $213442 */
    else if (mode == RIGHT) d3 = 0xf000;        /* $213482 */
    else if (mode == CENTRED) d3 = 0;           /* $2134be moveq #0,d3 */

    const uint16_t limit = f16(g, A3 + 0x2eaa);
    do {
        uint16_t d0 = f16(g, a4); a4 += 2;
        uint16_t d1 = f16(g, a4); a4 += 2;
        a4 += 2;
        uint16_t d5 = (uint16_t)(d1 >> 4);
        pf16(g, a4 - 4, d5);
        d1 = (uint16_t)(d1 + d1);
        d0 = (uint16_t)(d0 + d4);
        d0 = (uint16_t)-d0;
        int32_t prod = (int32_t)(int16_t)d0 * (int16_t)d1;
        d1 = (uint16_t)((uint32_t)prod >> 16);        /* muls; swap */
        d5 = (uint16_t)(d5 + d5);
        d5 = f16(g, a1 + (int16_t)d5);

        switch (mode) {
        case LEFT:                                    /* $213464 */
            d1 = (uint16_t)(d1 + d5);
            if ((int16_t)(d1 - d3) < 0) {             /* cmp/bpl */
                d3 = d1;
                d2 = f16(g, a4 - 2);
            }
            d1 = (uint16_t)(d1 - d5);
            d1 = (uint16_t)(d1 - d5);
            break;
        case RIGHT:                                   /* $2134a4 */
            d1 = (uint16_t)(d1 - d5);
            if ((int16_t)(d1 - d3) >= 0) {            /* cmp/bmi */
                d3 = d1;
                d2 = f16(g, a4 - 2);
            }
            break;
        case CENTRED: {                               /* $2134de */
            uint16_t t = (uint16_t)(f16(g, a4 - 4) >> 1);
            t = (uint16_t)-t;
            t = (uint16_t)(t + d7);
            if ((int16_t)(t - d3) >= 0) {
                d3 = t;
                d2 = f16(g, a4 - 2);
            }
            d1 = (uint16_t)(d1 - d5);
            break;
        }
        default:                                      /* $213520 */
            d1 = (uint16_t)(d1 - d5);
            break;
        }
        pf16(g, a4 - 6, d1);
        d7 = (uint16_t)(d7 + 1);
    } while (d7 != limit);

    if (mode == PLAIN) d2 = 0xff;                     /* $21352e */
    if (out_colour) *out_colour = d2;
    return d3;
}

/* $21337c-$21339c [snapshot-verified]: perspective pass for the near view. */
void road_perspective_near(Game *g, uint16_t d3_in)
{
    uint16_t colour = 0;
    uint16_t d3 = road_perspective(g, A3 - 0x2bd8, f16(g, A3 + 0x30dc),
                                   f16(g, A3 + 0x30e4),
                                   f16(g, A3 + 0x310a), d3_in, &colour);
    pf16(g, A3 + 0x2eb4, (uint16_t)(d3 + 0xa0));
    pf16(g, A3 + 0x2eb8, colour);
}

/* chip-RAM helpers: the sky pass builds copper list entries */
static inline uint16_t c16(Game *g, uint32_t a) { return g16(g->chip, a); }
static inline void pc16(Game *g, uint32_t a, uint16_t v) { p16(g->chip, a, v); }
static inline void pc8(Game *g, uint32_t a, uint8_t v) { g->chip[a] = v; }

/* $2136f6-$213eb2 [snapshot-verified]
 *
 * Sky and horizon copper builder.  Two Duff's-device chains, each entered
 * by a computed jump so that only the needed number of copies runs:
 *
 *   chain 1 ($21384a, 50 copies of 10 bytes) writes the sky gradient --
 *   one copper WAIT vpos byte plus one COLOR word per band, stepping the
 *   vpos by 4 lines and reading the colours from the table at $2c8a(A3).
 *
 *   chain 2 ($213a7a, 36 copies of 30 bytes) writes the horizon strip --
 *   per entry a WAIT vpos byte and three COLOR words looked up 8 bytes
 *   apart in the palette block at $2188(A3), stepping 16 bytes per entry.
 *
 * Which copper list is patched depends on $2f60(A3) (the buffer in use).
 * D6 == 0 skips the sky chain entirely and only repoints the list.
 */
void road_sky(Game *g)
{
    uint32_t a0, a1, a2, dlong;
    uint16_t sel = f16(g, A3 + 0x2f60);
    if (sel == 0) {
        a0 = 0x7f6d8; a1 = 0x7f874; a2 = 0x7f6c8; dlong = 0x7f868;
    } else if (sel == 1) {
        a0 = 0x7e510; a1 = 0x7e6ac; a2 = 0x7e500; dlong = 0x7e6a0;
    } else {
        a0 = 0x7edf4; a1 = 0x7ef90; a2 = 0x7ede4; dlong = 0x7ef84;
    }
    uint32_t a4 = A3 - 0x4180;
    uint16_t d4 = f16(g, A3 + 0x2eb0);
    uint16_t d5 = f16(g, A3 + 0x30fa);
    uint16_t d6 = f16(g, A3 + 0x30ec);
    uint16_t d7 = 0x2c;

    if (d6 == 0) {                                   /* $2137d2 */
        pc16(g, a2 + 6, (uint16_t)dlong);
        pc16(g, a2 + 2, (uint16_t)(dlong >> 16));
    } else {                                         /* $2137e4 */
        uint16_t d0 = (uint16_t)((int16_t)d5 >> 1);  /* asr.w #1 */
        d0 = (uint16_t)-d0;
        d0 = (uint16_t)(d0 + f16(g, A3 + 0x2eaa));
        d0 = (uint16_t)(d0 - 0x38);
        uint16_t d1 = (uint16_t)(d0 & 3);
        if (d1 != 0) d1 = (uint16_t)(d1 - 4);
        uint16_t d2 = (uint16_t)(d6 - d1);
        d2 = (uint16_t)((d2 + 3) >> 2);
        uint16_t d3 = (uint16_t)(0x32 - d2);
        a0 += (uint16_t)(d3 << 3);                   /* adda.w */
        pc16(g, a2 + 6, (uint16_t)a0);
        pc16(g, a2 + 2, (uint16_t)(a0 >> 16));

        d0 = (uint16_t)((d0 + 3) >> 2);
        d0 = (uint16_t)-d0;
        d0 = (uint16_t)(d0 + 0x4a);
        if ((int16_t)d0 < 0) d0 = 0;
        uint32_t src = f32(g, A3 + 0x2c8a);
        src += (uint32_t)(int32_t)(int16_t)d0;
        src += (uint32_t)(int32_t)(int16_t)d0;
        d1 = (uint16_t)(d1 + d7);

        for (uint16_t i = 0; i < d2; i++) {          /* $21384a x d2 */
            pc8(g, a0, (uint8_t)d1);
            pc16(g, a0 + 6, f16(g, src));
            src += 2;
            a0 += 8;
            d1 = (uint16_t)(d1 + 4);
        }
    }

    /* $213a42: the horizon strip, always run, with the entry A4/D4 */
    uint16_t d0 = (uint16_t)(d4 >> 1);               /* lsr.w #1 */
    uint16_t d1 = (uint16_t)(0x24 - d0);
    if ((int16_t)d1 < 0) d1 = 0;
    uint16_t runs = (uint16_t)(0x24 - d1);
    a1 += (uint16_t)(d1 << 4);
    pc16(g, dlong + 6, (uint16_t)a1);
    pc16(g, dlong + 2, (uint16_t)(a1 >> 16));

    const uint32_t pal = A3 + 0x2188;
    for (uint16_t i = 0; i < runs; i++) {            /* $213a7a x runs */
        pc8(g, a1, g->fast[a4 - GUEST_FAST_ADDR]);
        a4++;
        uint16_t idx = (uint16_t)(g->fast[a4 - GUEST_FAST_ADDR] & 0xff);
        a4++;
        idx = (uint16_t)(idx << 3);
        pc16(g, a1 + 0x6, f16(g, pal + idx));
        pc16(g, a1 + 0xa, f16(g, pal + idx + 2));
        pc16(g, a1 + 0xe, f16(g, pal + idx + 4));
        a1 += 0x10;
    }
}

/* $213534-$21365e [snapshot-verified]
 *
 * The band blitter: this is what actually paints the road.  For every
 * screen line from the horizon down it picks a pre-rendered road strip by
 * colour index, and blits it into the race bitmap with a horizontal
 * barrel shift taken from the line's edge position -- a shifted A->D copy
 * (BLTCON0 from the table at $216fb2, `$X9f0`, i.e. shift X with minterm
 * $f0 so D = A), BLTSIZE $0095 = 2 lines x 21 words, which is the race
 * screen's 42-byte row stride.
 *
 * It also maintains the colour-run list at A2: whenever the band colour
 * changes it appends (line, colour), which is what the copper builder
 * later turns into colour bands.
 *
 * Registers come from the caller ($2133be): A0 = $2f8e(A3) bitmap,
 * A4 = the edge stream, A2 = A3-$4180 run list, D1 = horizon line,
 * D2/D6 run-list seeds, D4/D5 = course position.  Returns the run-list
 * length in D0, which $2133c2 stores at $2eb0(A3).
 */
uint16_t road_bands(Game *g, Blitter *bl, uint32_t a0, uint32_t a4,
                    uint32_t a2, uint16_t d1, uint16_t d2, uint16_t d4,
                    uint32_t d5, uint16_t d6)
{
    const uint32_t a2_start = a2;

    uint32_t t = d5 << 2;                       /* asl.l #2 */
    t = (t >> 16) | (t << 16);                  /* swap */
    pf16(g, A3 + 0x2ea8, (uint16_t)t);
    a0 += 0x41a0;
    uint16_t d5w = (uint16_t)d5;
    d5w = (uint16_t)((d5w >> 4) | (d5w << 12)); /* ror.w #4 */
    uint16_t d3 = (uint16_t)(d5w & 0x3ff);
    d5w = 0x40;
    if (f16(g, A3 + 0x2df0) == 0) d5w = 0;

    d2 = (uint16_t)(d2 + d6);
    g->fast[a2 - GUEST_FAST_ADDR] = (uint8_t)d2; a2++;
    g->fast[a2 - GUEST_FAST_ADDR] = (uint8_t)d5w; a2++;

    bl->bltcon1 = 0;                            /* $42(A6) */
    bl->bltmod[3] = (int16_t)0x20a6;            /* BLTDMOD */
    bl->bltmod[0] = (int16_t)(uint16_t)(f32(g, A3 + 0x2d18) - 0x2a);

    const uint32_t a5 = A3 + 0x4e94;
    uint16_t d7 = d1;
    uint16_t d1x2 = (uint16_t)(d1 + d1);
    a4 += (uint32_t)(uint16_t)(d1x2 * 3);       /* three adda.w */
    a0 += (uint16_t)(d1x2 * 0x15);              /* mulu.w #$15 */
    d4 = (uint16_t)(d4 << 3);

    /* nibble table at A3-$3d8e from the band bytes at A3-$1e6e */
    {
        uint32_t p = A3 - 0x3d8e;
        uint32_t q = A3 - 0x1e6e;
        uint16_t sel = f16(g, A3 + 0x2ea8);
        q += (uint16_t)((sel << 2) & 0xfff0);
        for (int i = 0; i <= 8; i++) {
            for (int half = 0; half < 2; half++) {
                uint8_t v = g->fast[q - GUEST_FAST_ADDR];
                if (half == 0) q++;             /* (A1)+ then (A1) */
                v = (uint8_t)(v + v);
                v = (uint8_t)(v + 1);
                g->fast[p - GUEST_FAST_ADDR] = (uint8_t)(v >> 4); p++;
                g->fast[p - GUEST_FAST_ADDR] = v; p++;
            }
            q += 0xf;
        }
        pf16(g, A3 + 0x2ea8, (uint16_t)(f16(g, A3 + 0x2ea8) & 3));
    }

    const int variant_b = f16(g, A3 + 0x2df0) != 0;
    const uint16_t limit = f16(g, A3 + 0x2eaa);
    uint16_t d5run = d5w;

    do {
        uint16_t v = f16(g, a4); a4 += 2;
        uint16_t line = (uint16_t)(0xb1 + (uint16_t)-v);   /* neg; addi $b1 */
        uint16_t d0 = f16(g, a4); a4 += 2;
        uint16_t band = f16(g, a4); a4 += 2;
        band = (uint16_t)(band + d3);
        if ((int16_t)band < 0) band = 0x7fff;

        uint16_t extra = 0;
        if (variant_b) {                        /* $213674: banded variant */
            extra = (uint16_t)(band >> 7);
            extra = (uint16_t)(extra & 0xfff8);
            if ((int16_t)(extra - 0x40) >= 0) extra = 0x40;
        }
        band = (uint16_t)((int16_t)band >> 8);
        band = (uint16_t)((int16_t)band >> 2);
        band = (uint16_t)(band + f16(g, A3 + 0x2ea8));
        uint8_t colour = (uint8_t)
            (g->fast[(A3 - 0x3d8e + (int16_t)band) - GUEST_FAST_ADDR] & 0xf);
        if (variant_b) colour = (uint8_t)(colour + (uint8_t)extra);

        if (colour != (uint8_t)d5run) {         /* cmp.b: colour changed */
            d5run = colour;
            g->fast[a2 - GUEST_FAST_ADDR] = (uint8_t)d7; a2++;
            g->fast[a2 - 1 - GUEST_FAST_ADDR] =
                (uint8_t)(g->fast[a2 - 1 - GUEST_FAST_ADDR] + (uint8_t)d6);
            g->fast[a2 - GUEST_FAST_ADDR] = colour; a2++;
        }

        d0 = (uint16_t)(d0 << 2);
        uint32_t src = f32(g, a5 + (int16_t)d0);
        uint16_t shift = (uint16_t)((line + line) & 0x1e);
        uint16_t off = (uint16_t)((int16_t)line >> 3);
        src += (uint32_t)(int32_t)(int16_t)off;

        /* Writing BLTxPTL masks bit 0 -- blitter addresses are word
         * aligned in hardware, and the road's source offset (line >> 3)
         * is frequently odd, so without the mask every other strip reads
         * a byte-misaligned word and the road tears. */
        bl->bltpt[3] = a0 & ~1u;                /* BLTDPT */
        bl->bltpt[0] = src & ~1u;               /* BLTAPT */
        bl->bltcon0 = f16(g, 0x216fb2 + shift);
        blitter_run(bl, 0x95);                  /* BLTSIZE */

        a0 += 0x2a;
        d7 = (uint16_t)(d7 + 1);
    } while (d7 != limit);

    return (uint16_t)(a2 - a2_start);
}

/* ---- scenery iterators ($21508a's helpers) ---------------------------
 * Four variations on "advance to the next scenery item of one category,
 * or mark the category exhausted".  Each decrements its own countdown in
 * the base page, walks a record (a long distance plus two words), rejects
 * anything outside the visible distance window, and publishes the item's
 * scaled distance as a word pair.  They mutate the caller's pointer
 * register, so each returns the updated pointer.
 */

/* $215a7a: walks A2 DOWNWARD over packed words, skipping entries whose
 * high byte is 0 or >= $ff.  Countdown $2f32 steps by 4. */
uint32_t scen_next_a2(Game *g, uint32_t a2)
{
    for (;;) {
        uint16_t n = (uint16_t)(f16(g, A3 + 0x2f32) - 4);
        pf16(g, A3 + 0x2f32, n);
        if ((int16_t)n < 0) {
            pf16(g, A3 + 0x2f32, 0xffff);
            return a2;
        }
        a2 -= 2;
        uint16_t d0 = f16(g, a2);
        pf16(g, A3 + 0x2f2a, d0);
        d0 = (uint16_t)(d0 & 0xff00);
        if (d0 == 0) continue;
        if (d0 >= 0xff00) continue;             /* bcc: unsigned >= */
        return a2;
    }
}

/* $215a9c: walks A0 forward over 8-byte records; publishes to $2f34/$2f36
 * and the item's two words to $2f2e/$2eca.  Countdown $2f3c. */
uint32_t scen_next_a0(Game *g, uint32_t a0)
{
    for (;;) {
        uint16_t n = (uint16_t)(f16(g, A3 + 0x2f3c) - 1);
        pf16(g, A3 + 0x2f3c, n);
        if ((int16_t)n < 0) {
            pf16(g, A3 + 0x2f34, 0xffff);
            return a0;
        }
        uint32_t d0 = f32(g, a0); a0 += 4;
        pf16(g, A3 + 0x2f2e, f16(g, a0)); a0 += 2;
        pf16(g, A3 + 0x2eca, f16(g, a0)); a0 += 2;
        d0 -= f32(g, A3 + 0x2fc6);
        if ((int32_t)(d0 - 0x80000u) >= 0) continue;   /* cmp.l/bpl */
        d0 &= 0x3fffff;
        if (d0 >= 0x80000u) continue;                  /* cmp.l/bcc */
        d0 <<= 4;
        pf16(g, A3 + 0x2f36, (uint16_t)d0);
        pf16(g, A3 + 0x2f34, (uint16_t)(d0 >> 16));
        return a0;
    }
}

/* $215adc: indexes the table at A3-$3dce by its own countdown $2f10
 * rather than walking a pointer, so the caller's A0 is untouched. */
void scen_next_table(Game *g)
{
    for (;;) {
        uint16_t n = (uint16_t)(f16(g, A3 + 0x2f10) - 1);
        pf16(g, A3 + 0x2f10, n);
        if ((int16_t)n < 0) {
            pf16(g, A3 + 0x2f16, 0xffff);
            return;
        }
        uint32_t p = A3 - 0x3dce
                   + (uint32_t)(int32_t)(int16_t)(uint16_t)(n << 3);
        uint32_t d0 = f32(g, p); p += 4;
        pf16(g, A3 + 0x2f12, f16(g, p)); p += 2;
        pf16(g, A3 + 0x2f14, f16(g, p));
        d0 -= f32(g, A3 + 0x2fc6);
        d0 &= 0x3fffff;
        if (d0 >= 0x80000u) continue;
        d0 <<= 4;
        pf16(g, A3 + 0x2f18, (uint16_t)d0);
        pf16(g, A3 + 0x2f16, (uint16_t)(d0 >> 16));
        return;
    }
}

/* $215b24: like $215a9c but on A1, and a negative distance ends the
 * category outright rather than skipping the item.  Countdown $2f3e. */
uint32_t scen_next_a1(Game *g, uint32_t a1)
{
    for (;;) {
        uint16_t n = (uint16_t)(f16(g, A3 + 0x2f3e) - 1);
        pf16(g, A3 + 0x2f3e, n);
        if ((int16_t)n < 0) {
            pf16(g, A3 + 0x2f38, 0xffff);
            return a1;
        }
        uint32_t d0 = f32(g, a1); a1 += 4;
        pf16(g, A3 + 0x2f30, f16(g, a1)); a1 += 2;
        pf16(g, A3 + 0x2ec8, f16(g, a1)); a1 += 2;
        d0 -= f32(g, A3 + 0x2fc6);
        if ((int32_t)d0 < 0) {                  /* bmi $215b2a */
            pf16(g, A3 + 0x2f38, 0xffff);
            return a1;
        }
        if (d0 >= 0x80000u) continue;
        d0 <<= 4;
        pf16(g, A3 + 0x2f3a, (uint16_t)d0);
        pf16(g, A3 + 0x2f38, (uint16_t)(d0 >> 16));
        return a1;
    }
}

/* $215b58-$215bc8 [snapshot-verified]
 *
 * Sorts the four scenery categories by distance, nearest last.  Each
 * category is a (key, value) long pair; the routine is three passes of
 * three compare-and-exchange steps -- a full bubble sort of four items --
 * and `bcs` skipping the swap means the order it settles on is
 * DESCENDING by key.  The sorted eight longs are written to $2ecc(A3),
 * which is what the drawing code then walks.
 */
void scen_sort(Game *g)
{
    uint32_t k[4], v[4];
    k[0] = f32(g, A3 + 0x30d0); v[0] = f32(g, A3 + 0x30d4);
    k[1] = f32(g, A3 + 0x31a4); v[1] = f32(g, A3 + 0x31a8);
    k[2] = f32(g, A3 + 0x2e60); v[2] = f32(g, A3 + 0x2e64);
    k[3] = f32(g, A3 + 0x2e68); v[3] = f32(g, A3 + 0x2e6c);

    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < 3; i++)
            if (k[i + 1] >= k[i]) {             /* bcs skips: unsigned */
                uint32_t t = k[i]; k[i] = k[i + 1]; k[i + 1] = t;
                t = v[i]; v[i] = v[i + 1]; v[i + 1] = t;
            }

    uint32_t p = A3 + 0x2ecc;                   /* movem.l d0-d7,(a1) */
    for (int i = 0; i < 4; i++) {
        pf32(g, p, k[i]); p += 4;
        pf32(g, p, v[i]); p += 4;
    }
}
