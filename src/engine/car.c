/* car.c -- the car model: position integration, speed, surface lookup and
 * the engine-note ramp.
 *
 * $2129f2-$212ba2, called once per view block ($211e78 for the near view
 * at A3+$3054, $211ed4 for the split-screen one at A3+$3128).
 *
 * The course position the renderer reads is snapshotted through four
 * layers each frame ($3054 -> $305c -> $30aa -> $30b6 -> $30d8), so this
 * routine writes the ROOT: everything downstream is a copy.
 *
 * It also owns the engine sound: a Paula channel is claimed and released
 * through $dff000 here.  Audio is not modelled yet, so those register
 * writes are no-ops -- but every memory write the same paths make is
 * reproduced, because the snapshot gate covers them.
 */
#include <stdint.h>
#include "engine.h"

#define A3 0x208000u

/* muls.w: signed 16x16 -> 32 */
static int32_t muls_w(uint16_t a, uint16_t b)
{ return (int32_t)(int16_t)a * (int16_t)b; }

void car_update(Game *g, uint32_t a4)
{
    uint32_t d4 = f32(g, a4 + 0x04);            /* speed */
    uint32_t d7 = f32(g, a4 + 0x00);            /* position */
    uint16_t d0 = f16(g, a4 + 0xce);            /* per-frame delta */
    int32_t d0l = (int32_t)(int16_t)d0;         /* ext.l */
    if (d0l >= 0) d7 += (uint32_t)d0l;          /* bmi skips the add */
    pf32(g, a4 + 0x00, d7);                     /* <- the root write */

    if (f16(g, a4 + 0x26) != 0) {               /* $212a08 */
        uint16_t lim = f16(g, A3 + 0x2f92);
        if (d0 >= lim) {                        /* bcs skips */
            d0 = (uint16_t)(d0 + lim);
            d0 = (uint16_t)(d0 >> 1);
        }
    }
    {
        uint16_t lim = f16(g, A3 + 0x2f94);     /* $212a1a */
        if (d0 < lim) {                         /* bcc skips */
            d0 = (uint16_t)(d0 + lim);
            d0 = (uint16_t)(d0 >> 1);
        }
    }

    int32_t d1l = muls_w(d0, f16(g, a4 + 0x10));
    d4 += (uint32_t)d1l;
    d1l <<= 2;
    d4 += (uint32_t)d1l;

    /* ---- engine sound channel ($212a32-$212ae2) ---- */
    if (f16(g, A3 + 0x2dec) == 0) {
        int to_release = 0, to_claim = 0;
        if (f16(g, a4 + 0x1a) != 0) to_release = 1;
        else if (f16(g, a4 + 0x0e) != 0 && f16(g, a4 + 0xc4) != 0) {
            d1l = -1;
            to_claim = 1;
        } else {
            d1l = muls_w((uint16_t)(f16(g, a4 + 0x0e) - d0),
                         f16(g, a4 + 0x10));
            if (d1l < 0) d1l = -d1l;
            d1l -= 0x8000;
            if (d1l < 0) to_release = 1;
            else to_claim = 1;
        }
        if (to_claim) {                         /* $212a66 */
            if ((int16_t)f16(g, a4 + 0xb2) < 0) {
                if ((int16_t)f16(g, A3 + 0x2fa0) < 0x32) to_release = 1;
                else {
                    /* $20d7e8 allocates a voice and returns it in D0;
                     * with audio unmodelled there is no voice to take, so
                     * leave the field as the caller left it. */
                    to_claim = 0;
                }
            }
            if (to_claim) {                     /* $212a7e */
                uint16_t v = f16(g, a4 + 0xb2);
                /* lsr.w #8: a LOGICAL shift of the low word only, so a
                 * negative d1 becomes $00ff here, not $ffff */
                uint16_t vol = (uint16_t)((uint16_t)d1l >> 8);
                if ((int16_t)vol >= 0x40) vol = 0x40;
                pf16(g, a4 + 0xb4, vol);
                (void)v;                        /* AUDxVOL write: no audio */
            }
        }
        if (to_release) {                       /* $212aa8 */
            uint16_t v = f16(g, a4 + 0xb2);
            if ((int16_t)v >= 0) {
                pf16(g, a4 + 0xb2, 0xffff);
                pf16(g, a4 + 0xb4, 0);
                uint32_t p = A3 + 0x2ca2 + (uint32_t)(uint16_t)(v + v);
                pf16(g, p, 0xfffe);
                /* DMACON bit clear: no audio model */
            }
        }
    }

    /* ---- speed accumulation ($212ae4) ---- */
    uint16_t t = f16(g, a4 + 0x12);
    uint16_t s = (uint16_t)(t + t + t);
    s = (uint16_t)(s + s);
    s = (uint16_t)(s + s);
    d4 += (uint32_t)muls_w(s, f16(g, a4 + 0x0e));
    uint32_t hz = (uint32_t)f16(g, a4 + 0x1c) << 16;   /* swap; clr.w */
    d4 += (uint32_t)((int32_t)hz >> 1);                /* asr.l #1 */

    /* clamp the speed to +-$2c90(A3), comparing the swapped halves */
    {
        uint32_t sw = (d4 >> 16) | (d4 << 16);
        uint32_t lim = f32(g, A3 + 0x2c90);
        if ((int16_t)((uint16_t)sw - (uint16_t)lim) < 0)  /* cmp.w/bpl */
            sw = lim;
        uint32_t neg = (uint32_t)(-(int32_t)lim);
        if ((int16_t)((uint16_t)sw - (uint16_t)neg) >= 0) /* cmp.w/bmi */
            sw = neg;
        d4 = (sw >> 16) | (sw << 16);
        pf32(g, a4 + 0x04, d4);
    }

    /* ---- surface under the car, from the course table ($212b1e) ---- */
    uint16_t d3 = 0;
    uint16_t idx = (uint16_t)(f16(g, a4 + 0x00) << 4);
    uint16_t surf = f16(g, A3 - 0x1e78 + 0xa + (int16_t)idx);
    uint16_t lat = f16(g, a4 + 0x02);
    lat = (uint16_t)(lat >> 8);
    lat = (uint16_t)(lat >> 6);
    lat ^= 3;
    lat = (uint16_t)(lat << 2);
    surf = (uint16_t)(surf >> lat);
    surf = (uint16_t)(surf & 0xf);

    int rumble = 0;
    if (surf == 2) rumble = 1;
    else if (f16(g, A3 + 0x2dea) != 0 && surf == 1) {
        d3 = 0x400;
        rumble = 1;
    }
    if (rumble && f16(g, A3 + 0x2e00) != 0) {   /* $212b58 */
        uint16_t v = f16(g, a4 + 0x04);
        v = (uint16_t)(v + 0x200);
        v = (uint16_t)(v >> 4);
        uint8_t b = g->fast[(A3 + 0x2438 + (int16_t)v) - GUEST_FAST_ADDR];
        uint16_t d1w = (uint16_t)(b + b);
        d1w = (uint16_t)(d1w << 8);
        if ((int16_t)(d1w - d3) >= 0) d3 = d1w;   /* bmi skips */
    }
    pf16(g, a4 + 0xca, d3);

    /* ---- engine note ramps toward the target, $200 per frame ---- */
    uint16_t note = f16(g, a4 + 0xc8);
    if (note != d3) {
        if ((int16_t)(note - d3) < 0) {          /* bpl -> the other way */
            note = (uint16_t)(note + 0x200);
            if ((int16_t)(note - d3) >= 0) note = d3;
        } else {
            note = (uint16_t)(note - 0x200);
            if ((int16_t)(note - d3) < 0) note = d3;
        }
    }
    pf16(g, a4 + 0xc8, note);
}

/* $212680-$212708 [snapshot-verified]
 *
 * Checkpoint and lap marker.  Reads the course record under the car
 * (A3-$1e78 indexed by the position's high word << 4) and looks at the
 * marker byte pair at +$2: the value $7a00 is a checkpoint, which tops up
 * the time counter at +$20 (capped at $640000, reset value $630031) and
 * adds $30d40 to the distance at +$2e.  Also latches the record's
 * direction bit at +$a into +$24, and a $fc-tagged byte at +$72 into
 * +$b6.
 */
void car_checkpoint(Game *g, uint32_t a4)
{
    uint32_t d7 = f32(g, a4 + 0x08);
    d7 = (d7 >> 16) | (d7 << 16);               /* swap */
    d7 = (d7 & 0xffff0000u) | (uint16_t)(d7 << 4);
    const uint32_t a0 = A3 - 0x1e78;
    uint16_t rec = (uint16_t)d7;

    uint16_t d0 = f16(g, a0 + 2 + rec);
    uint16_t d1 = (uint16_t)(d0 & 0xff00);
    d0 = (uint16_t)(d0 - d1);
    if (d1 == 0x7a00) {                         /* checkpoint */
        if (f16(g, a4 + 0x18) == 0) {
            pf32(g, a4 + 0x2e, f32(g, a4 + 0x2e) + 0x30d40);
            pf16(g, a4 + 0xa8, 0x80);
            if (f16(g, A3 + 0x3038) == 2) d0 = 0;
            uint32_t d0l = (uint32_t)d0 << 16;  /* swap; clr.w */
            d0l += f32(g, a4 + 0x20);
            if (d0l >= 0x640000u) d0l = 0x630031u;   /* bcs skips */
            pf32(g, a4 + 0x20, d0l);
            pf16(g, a4 + 0x18, 1);
            pf16(g, a4 + 0x1a, 0);
        }
    } else {
        pf16(g, a4 + 0x18, 0);
    }

    d0 = f16(g, a0 + 0xa + rec);
    d0 = (uint16_t)(d0 & 0x8000);
    d0 = (uint16_t)((d0 << 1) | (d0 >> 15));    /* rol.w #1 */
    pf16(g, a4 + 0x24, d0);

    uint8_t b = g->fast[(a0 + 0x72 + rec) - GUEST_FAST_ADDR];
    if (b == 0xfc) {
        int8_t v = (int8_t)g->fast[(a0 + 0x73 + rec) - GUEST_FAST_ADDR];
        pf16(g, a4 + 0xb6, (uint16_t)(int16_t)v);
    }
}

/* $21263c-$212660 [snapshot-verified]: the race clock.  Every $32 frames
 * one unit comes off the time at +$20, unless the game is in mode 2, the
 * time has already run out, or the car is out of play (+$1a). */
void car_clock(Game *g, uint32_t a4)
{
    if (f16(g, A3 + 0x3038) == 2) return;
    if (f32(g, a4 + 0x20) == 0) return;
    if (f16(g, a4 + 0x1a) != 0) return;
    uint16_t t = (uint16_t)(f16(g, a4 + 0x22) - 1);
    pf16(g, a4 + 0x22, t);
    if ((int16_t)t >= 0) return;                /* bpl */
    pf16(g, a4 + 0x22, 0x31);
    pf16(g, a4 + 0x20, (uint16_t)(f16(g, a4 + 0x20) - 1));
}

/* $212662-$21267e [snapshot-verified]: distance travelled.  Adds
 * (speed^2 >> 16, rounded down to a multiple of 8) * 1.25 to +$2e. */
void car_distance(Game *g, uint32_t a4)
{
    uint16_t v = f16(g, a4 + 0x0e);
    uint32_t d0 = (uint32_t)v * v;              /* mulu.w */
    d0 = (d0 & 0xffff0000u);                    /* clr.w */
    d0 = (d0 >> 16) | (d0 << 16);               /* swap */
    d0 &= 0xfffffff8u;
    uint32_t d1 = d0 >> 2;                      /* lsr.l #2 */
    d0 += d1;
    pf32(g, a4 + 0x2e, f32(g, a4 + 0x2e) + d0);
}

/* $212bb8-$212c04 [snapshot-verified]
 *
 * Road shape under the car: reads four signed bytes out of the course
 * record (offsets 0, 1, $c, $d) and interpolates them by the position's
 * low nibble to give the near and far edge offsets.  D4 is taken BEFORE
 * the scaling, so it is the unscaled slope; D5 and D6 are the scaled and
 * negated edges.  Returns them through the three out pointers.
 */
static void car_road_shape(Game *g, uint32_t d0, uint16_t *out4,
                           uint16_t *out5, uint16_t *out6, uint32_t *out7)
{
    uint32_t d7 = (d0 >> 16) | (d0 << 16);      /* swap */
    d7 = (d7 & 0xffff0000u) | (uint16_t)(d7 << 4);
    uint16_t d3 = (uint16_t)((uint16_t)d0 >> 4);
    const uint32_t a0 = A3 - 0x1e78;
    uint16_t rec = (uint16_t)d7;

    uint8_t b = g->fast[(a0 + 1 + rec) - GUEST_FAST_ADDR];
    b = (uint8_t)(b + b);                       /* add.b */
    uint16_t d2 = (uint16_t)(int16_t)(int8_t)b; /* ext.w */
    int32_t p = (int32_t)(int16_t)d2 * (int16_t)d3;
    d2 = (uint16_t)((uint32_t)(p >> 8));        /* asr.l #8 */
    int8_t sb = (int8_t)g->fast[(a0 + 0xd + rec) - GUEST_FAST_ADDR];
    uint16_t d1 = (uint16_t)((uint16_t)(int16_t)sb << 5);
    d2 = (uint16_t)(d2 + d1);
    d2 = (uint16_t)-d2;
    *out5 = d2;

    sb = (int8_t)g->fast[(a0 + 0 + rec) - GUEST_FAST_ADDR];
    d2 = (uint16_t)(int16_t)sb;
    d2 = (uint16_t)(d2 + d2);
    *out4 = d2;                                 /* before the scaling */
    p = (int32_t)(int16_t)d2 * (int16_t)d3;
    d2 = (uint16_t)((uint32_t)(p >> 8));
    sb = (int8_t)g->fast[(a0 + 0xc + rec) - GUEST_FAST_ADDR];
    d1 = (uint16_t)((uint16_t)(int16_t)sb << 5);
    d2 = (uint16_t)(d2 + d1);
    d2 = (uint16_t)-d2;
    *out6 = d2;

    uint16_t lo = (uint16_t)d0;
    lo = (uint16_t)(lo >> 6);
    lo = (uint16_t)(lo >> 8);
    lo = (uint16_t)(lo + lo);
    d7 = (d7 & 0xffff0000u) | (uint16_t)(d7 + lo);
    *out7 = d7;
}

/* $212ba4-$212bb6 [snapshot-verified] */
void car_shape(Game *g, uint32_t a4)
{
    uint16_t d4, d5, d6;
    uint32_t d7;
    car_road_shape(g, f32(g, a4 + 0x08), &d4, &d5, &d6, &d7);
    pf16(g, a4 + 0x12, d4);
    pf16(g, a4 + 0x16, d5);
    pf16(g, a4 + 0x14, d6);
}

/* 68000 divu.w / divs.w: 32-bit dividend by 16-bit divisor, quotient in
 * the low word and remainder in the high word.  On overflow the 68000
 * sets V and leaves the destination UNCHANGED, which the game relies on
 * -- clamping code downstream assumes the old value survived. */
static int divu_w(uint32_t *d, uint16_t divisor)
{
    if (!divisor) return 0;                     /* trap; never happens here */
    uint32_t q = *d / divisor, r = *d % divisor;
    if (q > 0xffff) return 0;                   /* overflow: no write */
    *d = (r << 16) | (q & 0xffff);
    return 1;
}

static int divs_w(uint32_t *d, uint16_t divisor)
{
    if (!divisor) return 0;
    int32_t n = (int32_t)*d, dv = (int16_t)divisor;
    int32_t q = n / dv, r = n % dv;
    if (q > 32767 || q < -32768) return 0;
    *d = ((uint32_t)(uint16_t)r << 16) | (uint16_t)q;
    return 1;
}

/* $212734-$2129f0 [snapshot-verified]
 *
 * The handling model.  In order: settle the suspension bounce, read the
 * pad out of $36(A4) for steering trim, work the throttle/brake against
 * the gear ratio tables, rev-limit, then decide gear changes.
 *
 * The three table pointers are $3008 (brake curve), $300c (coast curve)
 * and $3010 (gear ratios); the current gear is D3, kept at +$28.  Speed
 * (D7, +$e) is carried as a 16.16 fraction through the divisions: the
 * dividend is always `speed << 16` built with ext.l/swap.
 */
void car_drive(Game *g, uint32_t a4)
{
    uint16_t d0, d1, d2, d3, d6, d7;
    uint32_t d0l, d4;

    /* ---- suspension bounce ($212734) ---- */
    if (f16(g, a4 + 0x40) != 0) {
        d1 = (uint16_t)-f16(g, a4 + 0xc8);
        pf16(g, a4 + 0x3e, (uint16_t)(f16(g, a4 + 0x3e) - 0x50));
        d0 = (uint16_t)(f16(g, a4 + 0x3e) + f16(g, a4 + 0x3c));
        if ((int16_t)(d0 - d1) < 0) {           /* bpl skips the landing */
            int land = 1;
            if (d1 == 0) {
                pf16(g, a4 + 0x3e, (uint16_t)-f16(g, a4 + 0x3e));
                pf16(g, a4 + 0x3e, (uint16_t)(f16(g, a4 + 0x3e) >> 1));
                if ((int16_t)(f16(g, a4 + 0x3e) - 0x100) >= 0)
                    land = 0;                   /* still bouncing */
            } else {
                pf16(g, a4 + 0xc2, 0);
                pf16(g, a4 + 0xc0, 1);
                /* $20d7e8 claims a voice for the landing thump: no audio */
            }
            if (land) {
                pf16(g, a4 + 0x40, 0);
                pf16(g, a4 + 0x3e, 0);
            }
            d0 = 0;
        }
        pf16(g, a4 + 0x3c, d0);
    } else {
        pf16(g, a4 + 0x3c, (uint16_t)-f16(g, a4 + 0xc8));
    }

    /* ---- $212792 ---- */
    d6 = f16(g, a4 + 0x10);
    d7 = f16(g, a4 + 0x0e);
    d4 = f32(g, a4 + 0x04);
    d3 = f16(g, a4 + 0x28);
    pf16(g, a4 + 0x26, 0);
    uint8_t pad = g->fast[(a4 + 0x36) - GUEST_FAST_ADDR];

    if (f16(g, a4 + 0x40) == 0) {
        if (pad & 0x01) d6 = (uint16_t)(d6 - f16(g, A3 + 0x2f96));
        if (pad & 0x02) d6 = (uint16_t)(d6 + f16(g, A3 + 0x2f96));

        /* speed / gear ratio -> engine revs */
        d0l = (uint32_t)(int32_t)(int16_t)d7;   /* ext.l */
        d0l = (d0l >> 16) | (d0l << 16);        /* swap */
        d2 = f16(g, f32(g, A3 + 0x3010) + (uint16_t)(d3 + d3));
        divu_w(&d0l, d2);
        if ((uint16_t)d0l >= 0x1f40) d0l = (d0l & 0xffff0000u) | 0x1f3f;

        uint32_t curve;
        if (f16(g, a4 + 0xbc) != 0) {           /* $2127e4: spin-out */
            pf16(g, a4 + 0xbc, (uint16_t)(f16(g, a4 + 0xbc) - 1));
            curve = 0x20eee6;                   /* lea (-$390a,PC) */
            pf16(g, a4 + 0x26, 0xffff);
        } else {
            curve = f32(g, A3 + 0x300c);        /* coast */
            int braking = f16(g, a4 + 0x38) != 0 ? (pad & 0x10) : (pad & 0x08);
            if (braking) {
                curve = f32(g, A3 + 0x3008);    /* accelerating */
                pf16(g, a4 + 0x26, 0xffff);
            }
        }

        d0 = (uint16_t)((uint16_t)d0l >> 7);    /* $212818 */
        d0 = (uint16_t)(d0 + d0);
        d0 = f16(g, curve + (int16_t)d0);
        d0l = (uint32_t)(int32_t)(int16_t)d0;   /* ext.l */
        uint32_t d1l = d0l;
        d0l += d1l;
        d0l += d1l;                             /* x3 */
        d0l <<= 8;
        d0l <<= 2;                              /* x1024 */
        d2 = (uint16_t)(d2 >> 1);
        divs_w(&d0l, d2);
        d7 = (uint16_t)(d7 + (uint16_t)d0l);

        /* rate-limit the delta the renderer consumes ($212832) */
        pf16(g, a4 + 0xc4, 0);
        d1 = (uint16_t)(d7 - f16(g, a4 + 0xce));
        if ((int16_t)(d1 - f16(g, A3 + 0x2f98)) < 0) {
            pf16(g, a4 + 0xce, d7);
        } else {
            pf16(g, a4 + 0xc4, 1);
            d1 = (uint16_t)((int16_t)d1 >> 5);
            pf16(g, a4 + 0xce, (uint16_t)(f16(g, a4 + 0xce) + d1));
            if ((int16_t)(f16(g, a4 + 0xce) - 0x1aa) < 0) {
                pf16(g, a4 + 0xce, 0x1aa);
                pf16(g, a4 + 0xc4, 0);
            }
        }
    }

    /* ---- steering settle ($212866) ---- */
    d6 = (uint16_t)(d6 + f16(g, a4 + 0x1e));
    if (d6 != 0) d6 = (uint16_t)((int16_t)d6 < 0 ? d6 + 1 : d6 - 1);
    if ((int16_t)(d6 - 0x20) >= 0) d6 = 0x20;
    if ((int16_t)(d6 - 0xffe0) < 0) d6 = 0xffe0;
    if (f16(g, a4 + 0x1e) != 0)
        pf16(g, a4 + 0x1e, (uint16_t)((int16_t)f16(g, a4 + 0x1e) < 0
                                      ? f16(g, a4 + 0x1e) + 1
                                      : f16(g, a4 + 0x1e) - 1));
    if (f16(g, a4 + 0x1c) != 0)
        pf16(g, a4 + 0x1c, (uint16_t)((int16_t)f16(g, a4 + 0x1c) < 0
                                      ? f16(g, a4 + 0x1c) + 1
                                      : f16(g, a4 + 0x1c) - 1));

    /* ---- surface drag and brake ($2128aa) ---- */
    if ((int16_t)f16(g, a4 + 0x3c) <= 0) {
        d0l = d4;
        d0l = (d0l >> 16) | (d0l << 16);        /* swap */
        d0 = (uint16_t)((uint16_t)d0l + 0x200);
        d0 = (uint16_t)(d0 >> 4);
        uint8_t drag = g->fast[(A3 + 0x23f8 + (int16_t)d0) - GUEST_FAST_ADDR];
        if (drag) {
            uint16_t f = (uint16_t)((uint16_t)(int16_t)(int8_t)drag << 6);
            uint32_t prod = (uint32_t)f * d7;   /* mulu.w */
            d7 = (uint16_t)(prod >> 16);        /* swap */
        }
        d0 = (uint16_t)((int16_t)f16(g, a4 + 0x16) >> 6);
        d7 = (uint16_t)(d7 + d0);
        if (pad & 0x04) d7 = (uint16_t)(d7 - 0x30);
    }
    if ((int16_t)(d7 - 0x1aa) < 0) d7 = 0x1aa;

    /* ---- revs from the new speed ($2128ec) ---- */
    d0l = (uint32_t)(int32_t)(int16_t)d7;
    d0l = (d0l >> 16) | (d0l << 16);
    d2 = f16(g, f32(g, A3 + 0x3010) + (uint16_t)(d3 + d3));
    divu_w(&d0l, d2);
    if ((int16_t)((uint16_t)d0l - 0x3e8) < 0) d0l = (d0l & 0xffff0000u) | 0x3e8;
    if ((int16_t)((uint16_t)d0l - 0x1f3f) >= 0)
        d0l = (d0l & 0xffff0000u) | 0x1f3f;
    pf16(g, A3 + 0x2fb2, (uint16_t)d0l);
    uint32_t prod = (uint32_t)(uint16_t)d0l * d2;
    d7 = (uint16_t)(prod >> 16);
    d1 = d3;

    /* ---- gear selection ($21291e) ---- */
    int shifted = 0;
    if (f16(g, a4 + 0x1a) == 0 && f16(g, a4 + 0xbc) == 0 &&
        f16(g, a4 + 0x3a) == 0) {
        uint8_t sticky = g->fast[(a4 + 0x34) - GUEST_FAST_ADDR];
        int manual = f16(g, a4 + 0x38) != 0;
        int up = manual ? (sticky & 0x08) : ((pad & 0x08) && (sticky & 0x10));
        if (up && d3 != 4) { d1 = (uint16_t)(d1 + 1); shifted = 1; }
        else if (!up) {
            int down = manual ? (sticky & 0x04)
                              : ((pad & 0x04) && (sticky & 0x10));
            if (down && d3 != 0) { d1 = (uint16_t)(d1 - 1); shifted = 1; }
        }
    } else {                                    /* $212986: automatic */
        d0 = f16(g, A3 + 0x2fb2);
        uint16_t prev = f16(g, a4 + 0x2c);
        if (d0 == prev || d0 < prev) {          /* beq / bcc */
            if (d0 < 0xdac && d1 != 0) { d1 = (uint16_t)(d1 - 1); shifted = 1; }
        } else {
            if (d0 >= 0x1388 && d1 != 4) { d1 = (uint16_t)(d1 + 1); shifted = 1; }
        }
    }

    if (shifted) {                              /* $2129ae: try the new gear */
        d0l = (uint32_t)(int32_t)(int16_t)d7;
        d0l = (d0l >> 16) | (d0l << 16);
        d2 = f16(g, f32(g, A3 + 0x3010) + (uint16_t)(d1 + d1));
        divu_w(&d0l, d2);
        if ((uint16_t)d0l >= 0x3e8 && (uint16_t)d0l < 0x1f3f) {
            pf16(g, A3 + 0x2fb2, (uint16_t)d0l);
            prod = (uint32_t)(uint16_t)d0l * d2;
            d7 = (uint16_t)(prod >> 16);
            d3 = d1;
            pf16(g, a4 + 0xce, d7);
        }
    }

    pf16(g, a4 + 0x28, d3);                     /* $2129de */
    pf16(g, a4 + 0x10, d6);
    pf16(g, a4 + 0x0e, d7);
    pf16(g, a4 + 0x2c, f16(g, A3 + 0x2fb2));
    (void)d4;
}

/* $21270a-$212732 [snapshot-verified]: run the handling model, then bleed
 * the speed off when the car is out of play (+$1a). */
void car_tick(Game *g, uint32_t a4)
{
    if (f16(g, a4 + 0x0e) == 0)
        g->fast[(a4 + 0x36) - GUEST_FAST_ADDR] = 0;
    if (f16(g, a4 + 0x1a) == 0) {
        car_drive(g, a4);
        return;
    }
    uint16_t saved = f16(g, a4 + 0x0e);
    car_drive(g, a4);
    pf16(g, a4 + 0x0e, saved);
    uint16_t v = (uint16_t)(f16(g, a4 + 0x0e) - 0x18);
    pf16(g, a4 + 0x0e, v);
    if ((int16_t)v < 0) pf16(g, a4 + 0x0e, 0);
}

/* $211dd4-$211e56 [snapshot-verified]
 *
 * Frame latch: copies the live car state of BOTH view blocks into the
 * shadow copies the renderer reads, along with four longs of scenery
 * state and a 10-entry table (6 bytes taken from each 16-byte source
 * record).  This is the first of the four stages that walk the car
 * position forward to $30d8 each frame.
 */
/* Returns the final A1.  The original leaves the destination pointer
 * advanced past the ten scenery records it copies, and a caller is
 * entitled to use it; a port that returns void models the memory and
 * loses the register, which is enough to disqualify it as a native
 * override.  See tools/override_check.py. */
uint32_t car_frame_latch(Game *g)
{
    pf32(g, A3 + 0x30a2, f32(g, A3 + 0x3054));
    pf32(g, A3 + 0x30a6, f32(g, A3 + 0x3102));
    pf32(g, A3 + 0x30aa, f32(g, A3 + 0x305c));
    pf16(g, A3 + 0x30ae, f16(g, A3 + 0x3060));
    pf16(g, A3 + 0x30b4, f16(g, A3 + 0x3078));
    pf16(g, A3 + 0x30b0, f16(g, A3 + 0x3068));
    pf16(g, A3 + 0x30b2, f16(g, A3 + 0x306a));
    pf32(g, A3 + 0x3176, f32(g, A3 + 0x3128));
    pf32(g, A3 + 0x317a, f32(g, A3 + 0x31d6));
    pf32(g, A3 + 0x317e, f32(g, A3 + 0x3130));
    pf16(g, A3 + 0x3182, f16(g, A3 + 0x3134));
    pf16(g, A3 + 0x3188, f16(g, A3 + 0x314c));
    pf16(g, A3 + 0x3184, f16(g, A3 + 0x313c));
    pf16(g, A3 + 0x3186, f16(g, A3 + 0x313e));
    pf32(g, A3 + 0x2e40, f32(g, A3 + 0x2e50));
    pf32(g, A3 + 0x2e44, f32(g, A3 + 0x2e54));
    pf32(g, A3 + 0x2e48, f32(g, A3 + 0x2e58));
    pf32(g, A3 + 0x2e4c, f32(g, A3 + 0x2e5c));
    uint32_t a0 = A3 - 0x3e46, a1 = A3 + 0x2e04;
    for (int i = 0; i <= 9; i++) {              /* dbra #$9 */
        pf32(g, a1, f32(g, a0)); a0 += 4; a1 += 4;
        pf16(g, a1, f16(g, a0)); a0 += 2; a1 += 2;
        a0 += 6;
    }
    return a1;
}

/* car_latch_gap -- the ONE copy still without a ported home.
 *
 * NOT a verified port: this is glue.  The game propagates the car state
 * forward through four separate routines, each of which does a handful of
 * copies among much other work:
 *
 *   $211058  $3054 -> $305c,  $3058 -> $3060
 *   $212d84  $30aa -> $30b6,  $30ae -> $30ba,  $30a2 -> $30d0
 *
 * $211dd4, $212cea and $212e58 are ported properly now (car_frame_latch,
 * race_frame_begin, race_frame_publish); these two sit inside routines
 * that are not ported yet -- $210ec4 (176 instructions) and the body of
 * $212cea that follows the setup.
 */
void car_latch_gap(Game *g)
{
    pf32(g, A3 + 0x305c, f32(g, A3 + 0x3054));      /* $211058 */
    pf16(g, A3 + 0x3060, f16(g, A3 + 0x3058));
    pf32(g, A3 + 0x30b6, f32(g, A3 + 0x30aa));      /* $212d84 */
    pf16(g, A3 + 0x30ba, f16(g, A3 + 0x30ae));
    pf32(g, A3 + 0x30d0, f32(g, A3 + 0x30a2));
}

/* $212cea-$212e34 [snapshot-verified]
 *
 * Race frame setup.  Rotates the TRIPLE buffer ($2f60 cycles 0,1,2 over
 * the three race bitmaps $1400 / $9742 / $11a84, publishing the draw
 * target at $2f8e and the display one at $2f8a), pushes a value through
 * an 8-entry ring at $3024 indexed by $3036, swaps the two blit queues
 * ($2f42 / $2f46), then latches both view blocks forward and copies the
 * 10-entry scenery table from $2e04 to A3-$4098 (6 bytes taken from each
 * source record, 8 written).
 */
void race_frame_begin(Game *g, uint16_t d0_in)
{
    pf16(g, A3 + 0x2fa8, f16(g, A3 + 0x2fa4));

    uint16_t d1 = (uint16_t)(f16(g, A3 + 0x2f60) + 1);
    if (d1 == 3) d1 = 0;
    pf16(g, A3 + 0x2f60, d1);
    if (d1 == 0) {
        pf32(g, A3 + 0x2f8e, 0x11a84);
        pf32(g, A3 + 0x2f8a, 0x1400);
    } else if (d1 == 1) {
        pf32(g, A3 + 0x2f8e, 0x1400);
        pf32(g, A3 + 0x2f8a, 0x9742);
    } else {
        pf32(g, A3 + 0x2f8e, 0x9742);
        pf32(g, A3 + 0x2f8a, 0x11a84);
    }

    /* the 8-entry ring at $3024 */
    uint32_t a1 = A3 + 0x3024;
    uint16_t k = (uint16_t)((f16(g, A3 + 0x3036) - 1) & 7);
    uint16_t d0 = (uint16_t)(d0_in + f16(g, a1 + (uint16_t)(k + k)));
    k = f16(g, A3 + 0x3036);
    pf16(g, a1 + (uint16_t)(k + k), d0);
    pf16(g, A3 + 0x3036, (uint16_t)((f16(g, A3 + 0x3036) + 1) & 7));

    pf16(g, A3 + 0x2fa6, 0);
    uint32_t q = f32(g, A3 + 0x2f46);           /* swap the blit queues */
    pf32(g, A3 + 0x2f46, f32(g, A3 + 0x2f42));
    pf32(g, A3 + 0x2f42, q);

    for (int v = 0; v < 2; v++) {               /* both view blocks */
        uint32_t a4 = A3 + (v ? 0x3128 : 0x3054);
        pf32(g, a4 + 0x62, f32(g, a4 + 0x56));
        pf16(g, a4 + 0x66, f16(g, a4 + 0x5a));
        pf32(g, a4 + 0x7c, f32(g, a4 + 0x4e));
        pf32(g, a4 + 0x80, f32(g, a4 + 0x52));
        pf16(g, a4 + 0x78, f16(g, a4 + 0x60));
        pf16(g, a4 + 0x8e, f16(g, a4 + 0x5c));
        pf16(g, a4 + 0x8c, f16(g, a4 + 0x5e));
    }

    pf32(g, A3 + 0x2e60, f32(g, A3 + 0x2e40));
    pf32(g, A3 + 0x2e64, f32(g, A3 + 0x2e44));
    pf32(g, A3 + 0x2e68, f32(g, A3 + 0x2e48));
    pf32(g, A3 + 0x2e6c, f32(g, A3 + 0x2e4c));

    uint32_t a0 = A3 + 0x2e04;
    uint32_t dst = A3 - 0x4098;
    for (int i = 0; i < 10; i++) {              /* unrolled x10 */
        pf32(g, dst, f32(g, a0)); a0 += 4; dst += 4;
        pf16(g, dst, f16(g, a0)); a0 += 2; dst += 2;
        if (i != 9) dst += 2;                   /* addq.w #2,A1 between */
    }
}

/* $212e58-$212e76 [snapshot-verified]: the final latch, moving each view
 * block's position into the field the renderer reads ($30d8 / $31ac). */
void race_frame_publish(Game *g)
{
    for (int v = 0; v < 2; v++) {
        uint32_t a4 = A3 + (v ? 0x3128 : 0x3054);
        pf32(g, a4 + 0x84, f32(g, a4 + 0x62));
        pf16(g, a4 + 0x88, f16(g, a4 + 0x66));
    }
}
