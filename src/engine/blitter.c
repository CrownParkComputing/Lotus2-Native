/* blitter.c -- see blitter.h.  Ported from the host's blit(); the two are
 * kept behaviourally identical so a native blit can be gated byte-for-byte
 * against an oracle snapshot. */
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
