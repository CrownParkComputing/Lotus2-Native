/* blitter.c -- see blitter.h.  Ported from the host's blit(); the two are
 * kept behaviourally identical so a native blit can be gated byte-for-byte
 * against an oracle snapshot. */
#include "engine.h"
#include "blitter.h"

static uint16_t minterm(uint8_t function, uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t d = 0;
    if (function & 0x01) d |= (uint16_t)(~a & ~b & ~c);
    if (function & 0x02) d |= (uint16_t)(~a & ~b &  c);
    if (function & 0x04) d |= (uint16_t)(~a &  b & ~c);
    if (function & 0x08) d |= (uint16_t)(~a &  b &  c);
    if (function & 0x10) d |= (uint16_t)( a & ~b & ~c);
    if (function & 0x20) d |= (uint16_t)( a & ~b &  c);
    if (function & 0x40) d |= (uint16_t)( a &  b & ~c);
    if (function & 0x80) d |= (uint16_t)( a &  b &  c);
    return d;
}

static uint16_t rw(const Blitter *b, uint32_t a)
{
    a &= b->chip_size - 1;
    return (uint16_t)((b->chip[a] << 8) |
                      b->chip[(a + 1) & (b->chip_size - 1)]);
}

static void ww(Blitter *b, uint32_t a, uint16_t v)
{
    a &= b->chip_size - 1;
    b->chip[a] = (uint8_t)(v >> 8);
    b->chip[(a + 1) & (b->chip_size - 1)] = (uint8_t)v;
}

void blitter_run(Blitter *b, uint16_t size)
{
    int height = (size >> 6) & 0x3ff;
    int width = size & 0x3f;
    if (!height) height = 1024;
    if (!width) width = 64;

    int ashift = (b->bltcon0 >> 12) & 15;
    int bshift = (b->bltcon1 >> 12) & 15;
    bool usea = b->bltcon0 & 0x0800;
    bool useb = b->bltcon0 & 0x0400;
    bool usec = b->bltcon0 & 0x0200;
    bool used = b->bltcon0 & 0x0100;
    bool descending = b->bltcon1 & 2;
    int step = descending ? -2 : 2;

    b->blt_zero = true;
    for (int y = 0; y < height; y++) {
        uint32_t aprevious = 0, bprevious = 0;
        for (int x = 0; x < width; x++) {
            uint16_t araw = usea ? rw(b, b->bltpt[0]) : b->bltdat[0];
            uint16_t braw = useb ? rw(b, b->bltpt[1]) : b->bltdat[1];
            uint16_t c = usec ? rw(b, b->bltpt[2]) : b->bltdat[2];
            if (x == 0) araw &= b->bltafwm;
            if (x == width - 1) araw &= b->bltalwm;
            uint16_t a, bb;
            if (!descending) {
                a = (uint16_t)(((aprevious << 16) | araw) >> ashift);
                bb = (uint16_t)(((bprevious << 16) | braw) >> bshift);
            } else {
                a = ashift ? (uint16_t)((((uint32_t)araw << 16) |
                                         aprevious) >> (16 - ashift)) : araw;
                bb = bshift ? (uint16_t)((((uint32_t)braw << 16) |
                                          bprevious) >> (16 - bshift)) : braw;
            }
            aprevious = araw;
            bprevious = braw;
            uint16_t d = minterm((uint8_t)(b->bltcon0 & 0xff), a, bb, c);
            if (d) b->blt_zero = false;
            if (usea) b->bltpt[0] = (uint32_t)(b->bltpt[0] + step);
            if (useb) b->bltpt[1] = (uint32_t)(b->bltpt[1] + step);
            if (usec) b->bltpt[2] = (uint32_t)(b->bltpt[2] + step);
            if (used) {
                ww(b, b->bltpt[3], d);
                b->bltpt[3] = (uint32_t)(b->bltpt[3] + step);
            }
        }
        for (int ch = 0; ch < 4; ch++) {
            bool on = ch == 0 ? usea : ch == 1 ? useb : ch == 2 ? usec : used;
            if (on)
                b->bltpt[ch] = (uint32_t)(b->bltpt[ch] +
                                          (descending ? -b->bltmod[ch]
                                                      : b->bltmod[ch]));
        }
    }
    b->blits++;
}

/* ---- the blit queue --------------------------------------------------
 * The game appends records to a queue and lets the blitter-done
 * interrupt walk them, one blit per interrupt, so the frame's drawing is
 * spread across the frame.  A native runner needs none of that: the
 * chain exists to fit the work into a 7 MHz 68000's spare cycles, not
 * because anything depends on the order being interleaved with the
 * raster.  So this walks the same records and runs the same blits, back
 * to back.
 *
 * The record format is defined by those handlers and nowhere else, so
 * the field table is generated from them rather than transcribed --
 * see tools/blitq_types.py and re/BLITQUEUE.md.
 */
#include "blitq_types.h"

/* Set by the gate to record where each typed record starts, so the
 * parse can be compared against the addresses a real drain visited. */
#define BLITQ_TRACE_MAX 256
uint32_t *blitq_trace;
int blitq_trace_n;

static void blitq_poke(Blitter *b, unsigned reg, uint32_t v, int wide)
{
    switch (reg) {
    case 0x40:                                  /* BLTCON0 (+BLTCON1) */
        if (wide) { b->bltcon0 = (uint16_t)(v >> 16);
                    b->bltcon1 = (uint16_t)v; }
        else        b->bltcon0 = (uint16_t)v;
        break;
    case 0x42: b->bltcon1 = (uint16_t)v; break;
    case 0x44:                                  /* BLTAFWM (+BLTALWM) */
        if (wide) { b->bltafwm = (uint16_t)(v >> 16);
                    b->bltalwm = (uint16_t)v; }
        else        b->bltafwm = (uint16_t)v;
        break;
    case 0x46: b->bltalwm = (uint16_t)v; break;
    case 0x48: b->bltpt[2] = v & 0x1ffffe; break;          /* BLTCPT */
    case 0x4c: b->bltpt[1] = v & 0x1ffffe; break;          /* BLTBPT */
    case 0x50: b->bltpt[0] = v & 0x1ffffe; break;          /* BLTAPT */
    case 0x54: b->bltpt[3] = v & 0x1ffffe; break;          /* BLTDPT */
    case 0x60:                                  /* BLTCMOD (+BLTBMOD) */
        if (wide) { b->bltmod[2] = (int16_t)(v >> 16);
                    b->bltmod[1] = (int16_t)v; }
        else        b->bltmod[2] = (int16_t)v;
        break;
    case 0x62: b->bltmod[1] = (int16_t)v; break;
    case 0x64:                                  /* BLTAMOD (+BLTDMOD) */
        if (wide) { b->bltmod[0] = (int16_t)(v >> 16);
                    b->bltmod[3] = (int16_t)v; }
        else        b->bltmod[0] = (int16_t)v;
        break;
    case 0x66: b->bltmod[3] = (int16_t)v; break;
    case 0x70: b->bltdat[2] = (uint16_t)v; break;          /* BLTCDAT */
    case 0x72: b->bltdat[1] = (uint16_t)v; break;          /* BLTBDAT */
    case 0x74: b->bltdat[0] = (uint16_t)v; break;          /* BLTADAT */
    default: break;
    }
}

/* The queue opens with a PROLOGUE the typed dispatcher never sees: the
 * road's own records, consumed by the unrolled handlers at $216fd2
 * before control reaches $21718a.  Four sections, each ended early by a
 * negative long where a BLTDPT is expected -- which is the $ffffffff
 * road_blitqueue() writes when a band's enable word is zero.
 *
 * Returns where the typed records begin.
 */
static uint32_t blitq_prologue(Game *g, Blitter *b, uint32_t q, int *done)
{
    /* horizon strip: two plain records, no sentinel */
    for (int i = 0; i < 2; i++) {
        blitq_poke(b, 0x54, f32(g, q), 1); q += 4;
        blitter_run(b, f16(g, q)); q += 2; (*done)++;
    }
    /* the $30f0 band: four, the first guarded */
    for (int i = 0; i < 4; i++) {
        uint32_t d = f32(g, q); q += 4;
        if (i == 0 && (int32_t)d < 0) break;          /* bmi $21708a */
        blitq_poke(b, 0x54, d, 1);
        blitter_run(b, f16(g, q)); q += 2; (*done)++;
    }
    /* the $30e8 shadow band: four wide records, the first carrying a
     * BLTCON0 of its own */
    for (int i = 0; i < 4; i++) {
        uint32_t d = f32(g, q); q += 4;
        if (i == 0 && (int32_t)d < 0) break;          /* bmi $21710e */
        blitq_poke(b, 0x54, d, 1);
        blitq_poke(b, 0x50, f32(g, q), 1); q += 4;
        if (i == 0) { blitq_poke(b, 0x40, f16(g, q), 0); q += 2; }
        blitter_run(b, f16(g, q)); q += 2; (*done)++;
    }
    /* the $30ee band */
    for (int i = 0; i < 4; i++) {
        uint32_t d = f32(g, q); q += 4;
        if (i == 0 && (int32_t)d < 0) break;          /* bmi $21718a */
        blitq_poke(b, 0x54, d, 1);
        blitter_run(b, f16(g, q)); q += 2; (*done)++;
    }
    return q;
}

/* The typed section on its own: a stream of [type word][body] ending at
 * a zero type.  The preview builds a queue of its own and has no road
 * prologue in front of it. */
int blitq_run_records(Game *g, Blitter *b, uint32_t queue)
{
    int done = 0;
    /* The queue is 2 KB of records at most; the count is a backstop
     * against a stale read pointer, not a real limit. */
    for (int guard = 0; guard < 4096; guard++) {
        if (blitq_trace && blitq_trace_n < BLITQ_TRACE_MAX)
            blitq_trace[blitq_trace_n++] = queue;   /* A5 at the dispatcher */
        uint16_t type = f16(g, queue);
        queue += 2;
        if (type == 0) break;                   /* tst.w (A5)+ ; beq */

        const BlitqType *t = NULL;
        for (int i = 0; i < BLITQ_TYPE_COUNT; i++)
            if (blitq_types[i].type == type) { t = &blitq_types[i]; break; }
        if (!t) break;                          /* unknown type: the
                                                   dispatcher falls through */

        for (int i = 0; i < t->count; i++) {
            const BlitqField *f = &t->field[i];
            uint32_t v;
            if (f->src == 0) {
                v = f->value;                   /* the handler's own constant */
            } else {
                v = f->wide ? f32(g, queue) : f16(g, queue);
                queue += f->wide ? 4 : 2;
            }
            if (f->reg == 0x58) {               /* BLTSIZE starts the blit */
                blitter_run(b, (uint16_t)v);
                done++;
            } else {
                blitq_poke(b, f->reg, v, f->wide);
            }
        }
    }
    return done;
}

int blitq_run(Game *g, Blitter *b, uint32_t queue)
{
    int done = 0;
    queue = blitq_prologue(g, b, queue, &done);
    return done + blitq_run_records(g, b, queue);
}
