/* input.c -- the pad decoder ($211770-$211850).
 *
 * Reads both joystick ports and publishes three bytes per port into the
 * base page, plus a merged pair:
 *
 *   port 1 (JOY1DAT, fire = CIA-A PRA bit 7):  $308a live, $3086 prev,
 *                                              $3088 newly pressed
 *   port 0 (JOY0DAT, fire = CIA-A PRA bit 6):  $315e / $315a / $315c
 *   merged:  $2faa = edges of both, $2fac = live of both
 *
 * `$308a(A3)` is the same address as `$36(A4)` in the near car block, so
 * the handling model reads the pad straight out of its own structure.
 *
 * Bit layout of the decoded byte, as the car model uses it:
 *   0 left   1 right   2 down/brake   3 up/accelerate   4 fire
 *
 * The direction decode is the standard Amiga quadrature read: bits 1 and
 * 9 of JOYxDAT give right and left directly, and XORing the word with
 * itself shifted left one bit recovers down and up.
 *
 * Inputs come from Input rather than from hardware, so the frontend can
 * drive the engine from anything -- a real pad, a replay, or a test.
 */
#include <stdint.h>
#include "engine.h"

#define A3 0x208000u

static uint16_t decode_stick(uint16_t dat, int fire_pressed)
{
    uint16_t d1 = 0;
    if (dat & 0x0002) d1 |= 1u << 1;            /* right */
    if (dat & 0x0200) d1 |= 1u << 0;            /* left */
    uint16_t x = (uint16_t)(dat ^ (uint16_t)(dat << 1));
    if (x & 0x0002) d1 |= 1u << 2;              /* down */
    if (x & 0x0200) d1 |= 1u << 3;              /* up */
    if (fire_pressed) d1 |= 1u << 4;            /* CIA bit clear = pressed */
    return d1;
}

/* publish live/previous/edge for one port */
static void publish(Game *g, uint32_t live, uint32_t prev, uint32_t edge,
                    uint8_t state)
{
    uint8_t was = g->fast[prev - GUEST_FAST_ADDR];
    g->fast[live - GUEST_FAST_ADDR] = state;
    uint8_t now = g->fast[live - GUEST_FAST_ADDR];
    uint8_t pressed = (uint8_t)((was ^ now) & now);
    g->fast[prev - GUEST_FAST_ADDR] = now;
    g->fast[edge - GUEST_FAST_ADDR] = pressed;
}

void input_read(Game *g, const Input *in)
{
    uint8_t p1 = (uint8_t)decode_stick(in->joy1dat, !(in->cia_pra & 0x80));
    publish(g, A3 + 0x308a, A3 + 0x3086, A3 + 0x3088, p1);

    /* $2117ce: a demo/replay source overrides port 0 */
    uint8_t p0;
    if (f16(g, A3 + 0x2f5e) != 0)
        p0 = g->fast[(A3 + 0x2f5c) - GUEST_FAST_ADDR];
    else
        p0 = (uint8_t)decode_stick(in->joy0dat, !(in->cia_pra & 0x40));
    publish(g, A3 + 0x315e, A3 + 0x315a, A3 + 0x315c, p0);

    uint8_t both_edge = (uint8_t)(g->fast[(A3 + 0x3088) - GUEST_FAST_ADDR] |
                                  g->fast[(A3 + 0x315c) - GUEST_FAST_ADDR]);
    g->fast[(A3 + 0x2faa) - GUEST_FAST_ADDR] = both_edge;
    uint8_t both_live = (uint8_t)(g->fast[(A3 + 0x308a) - GUEST_FAST_ADDR] |
                                  g->fast[(A3 + 0x315e) - GUEST_FAST_ADDR]);
    g->fast[(A3 + 0x2fac) - GUEST_FAST_ADDR] = both_live;
}
