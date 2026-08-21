/* engine.h -- native Lotus Turbo Challenge 2 engine: a C rendering of the
 * phase-direct kernel (see re/ARCH.md, re/VERBS.md).  There is no object
 * verb table in this engine: a sequencer drives the game by writing phase
 * + params into the base page and parking on the frame tick, and VBLANK
 * runs the fixed tail (PT-replay interface, music).  Gameplay is the road
 * interpolator + blitter road bands + sprite/scenery passes.
 *
 * TRANSLATE convention: the 68000 base page (A3 = $208000) and chip RAM
 * stay guest byte images (guest.h), so every ported routine is diffable
 * word-for-word against the oracle dumps in re/pipeline/.  Named field
 * macros below wrap the hot offsets; add names as routines are ported.
 *
 * Units: PAL 320x200, bitplane stride 8000 bytes, 50 Hz VBL.  One phase =
 * one screen state; frame phases alternate 0x11/0x13 (double-buffered).
 */
#ifndef LOTUS2_ENGINE_H
#define LOTUS2_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "guest.h"

#define LOTUS2_SCREEN_W 320
#define LOTUS2_SCREEN_H 200
#define LOTUS2_PLANE_STRIDE 0x1f40
#define LOTUS2_MAX_PLANES 5

/* phase values observed in g2fe0 (extend as the sequencer is decoded) */
enum {
    PHASE_FRAME_A = 0x11,   /* frame phase, buffer A active */
    PHASE_FRAME_B = 0x13,   /* frame phase, buffer B active */
    PHASE_STEP_10 = 0x10,   /* sequence steps seen in the attract log */
    PHASE_STEP_18 = 0x18,
    PHASE_STEP_34 = 0x34,
};

/* base-page offsets (re/ARCH.md "Registers / globals") */
enum {
    G_PHASE_IDX   = 0x2fc0,  /* VBLANK param table index */
    G_SCREEN_A    = 0x2fd6,  /* long: front chip buffer ($10186) */
    G_SCREEN_B    = 0x2fda,  /* long: back chip buffer ($1bd06) */
    G_PHASE       = 0x2fe0,
    G_TICK        = 0x2fee,  /* frame tick set by VBLANK; parked on */
    G_EXIT_SEQ    = 0x2ff0,
    G_BLITQ_IN    = 0x2fa4,
    G_BLITQ_OUT   = 0x2fa8,
    G_PAL_BANK    = 0x3000,  /* fade bank selector for load_palette */
    G_PALETTE     = 0x320c,  /* 32 RGB4 words: master palette */
};

/* chip-RAM addresses the display verbs touch (re/ARCH.md memory map) */
enum {
    CHIP_FADE_BANKS   = 0x5400,  /* 9 banks x 32 words built by build_fade */
    CHIP_COP_BPLCON0  = 0x7ff22, /* copper MOVE operand: BPLCON0 */
    CHIP_COP_BPLPTRS  = 0x7ff44, /* copper MOVEs: BPLxPTH/L pairs */
    CHIP_COP_PALETTE  = 0x7ff74, /* copper MOVEs: COLOR00..31 */
    CHIP_COPLIST_1    = 0x7f5f0, /* the two COP1LC targets seen in trace */
    CHIP_COPLIST_2    = 0x7fedc,
};

#define GUEST_FAST_ADDR 0x200000u   /* ExpMem window */
#define GUEST_FAST_SIZE 0x890000u

/* What the engine needs from the outside world each frame: the two
 * joystick words as the hardware would report them, and CIA-A PRA whose
 * bits 6 and 7 are the two fire buttons (active low). */
typedef struct {
    uint16_t joy0dat, joy1dat;
    uint8_t  cia_pra;
} Input;

typedef struct {
    /* Optional chipset write.  The ports were built against flat memory
     * images, where a write to a custom register has nowhere to go, so
     * several were simply left out -- AUDxVOL and DMACON among them.
     * That is fine for a snapshot gate and wrong the moment a port
     * REPLACES the routine, because the game stops setting channel
     * volumes.  NULL keeps the old behaviour for the offline harness;
     * the native build points it at the host's chipset. */
    void (*poke)(uint32_t addr, uint16_t value);
    uint8_t *chip;   /* guest chip RAM image (GUEST_CHIP_SIZE) */
    uint8_t *base;   /* guest base page window ($208000, GUEST_BASE_SIZE) */
    uint8_t *fast;   /* full ExpMem image ($200000+GUEST_FAST_SIZE) or NULL;
                        when set, `base` points at fast + $8000 */
} Game;

/* fast-image accessors taking RUNTIME addresses ($20xxxx) */
static inline uint16_t f16(const Game *g, uint32_t addr)
{ return g16(g->fast, addr - GUEST_FAST_ADDR); }
static inline uint32_t f32(const Game *g, uint32_t addr)
{ return g32(g->fast, addr - GUEST_FAST_ADDR); }
static inline void pf16(Game *g, uint32_t addr, uint16_t v)
{ p16(g->fast, addr - GUEST_FAST_ADDR, v); }
static inline void pf32(Game *g, uint32_t addr, uint32_t v)
{ p32(g->fast, addr - GUEST_FAST_ADDR, v); }

/* ---- gameplay: road pipeline stages (re/pipeline/disasm/road_213534.txt,
 * driver chain at $212f12; each stage is snapshot-verified) ---- */
void road_interpolate(Game *g, int which);   /* $214268 / $21427a */
void road_blitqueue(Game *g);                /* $2143c2 */
void road_band_bounds(Game *g, uint32_t view);  /* $214354; $214344/$21434c */
uint16_t road_keyframes(Game *g, uint32_t a2, uint32_t course_pos,
                        uint16_t *out_line);   /* $213edc */
void road_keyframes_near(Game *g);             /* $213eb4 */
void road_perspective_near(Game *g, uint16_t d3_in);  /* $21337c; $213416 */
void road_sky(Game *g);                        /* $2136f6 */
#include "blitter.h"
uint32_t scen_next_a2(Game *g, uint32_t a2);   /* $215a7a */
uint32_t scen_next_a0(Game *g, uint32_t a0);   /* $215a9c */
void     scen_next_table(Game *g);             /* $215adc */
uint32_t scen_next_a1(Game *g, uint32_t a1);   /* $215b24 */
uint32_t scen_sort(Game *g, uint32_t regs[8]);  /* $215b58; D0-D7, ret A1 */
int      scen_prepare(Game *g, uint32_t *out_a0, uint32_t *out_a2); /* $21508a */

/* The scenery drawing routines are register machines; whole 32-bit
 * registers are carried because `move.w` leaves the high half alone and
 * the snapshot gate compares all 32 bits. */
typedef struct {
    uint32_t d0, d1, d2, d3, d4, d5, d6, d7;
    uint32_t a0, a1, a2, a4;
} Span;
void span_fill(Game *g, Span *s);              /* $2169e0 */
void scen_project(Game *g, Span *s);           /* $2160f2 -> $216346 */
void scen_emit(Game *g, Span *s);              /* $216346 */
uint32_t scen_shape_ptr(Game *g, uint32_t a1, uint32_t d6);  /* $215dac */

/* The 68000 register file, for ports that must reproduce a routine's
 * effect on registers and not only on memory -- the condition for
 * replacing that routine outright.  See tools/override_check.py. */
typedef struct { uint32_t d[8], a[8]; } Regs;

/* word ops leave the upper half of a data register alone */
static inline uint16_t w(uint32_t r) { return (uint16_t)r; }
static inline uint32_t setw(uint32_t r, uint16_t v)
{ return (r & 0xffff0000u) | v; }

/* Range-aware guest read: the sound tables hold CHIP pointers, and a
 * fast-only accessor underflows on those.  Used where a pointer's origin
 * is not known statically. */
static inline uint16_t m16(const Game *g, uint32_t addr)
{
    return addr < GUEST_CHIP_SIZE ? g16(g->chip, addr)
                                  : g16(g->fast, addr - GUEST_FAST_ADDR);
}
static inline uint32_t m32(const Game *g, uint32_t addr)
{
    return addr < GUEST_CHIP_SIZE ? g32(g->chip, addr)
                                  : g32(g->fast, addr - GUEST_FAST_ADDR);
}

/* ---- the blit queue ---- */
/* Walk the records at `queue` and run every blit through `b`.  Returns
 * how many blits were run.  See re/BLITQUEUE.md. */
int blitq_run(Game *g, Blitter *b, uint32_t queue);
int blitq_run_records(Game *g, Blitter *b, uint32_t queue);
extern uint32_t *blitq_trace;   /* gate hook: record starts, or NULL */
extern int blitq_trace_n;

/* ---- weather (the courses other than FOREST) ---- */
void     weather_span(Game *g, Regs *r);   /* $21495a */
uint32_t weather_emit(Game *g, Regs *r);   /* $214994; returns A4 */
void     weather_band(Game *g, Regs *r, int which);  /* $2148b2/$2148da/$214914 */
void     weather_step(Game *g, Regs *r);   /* $215906 */
void     weather_pass(Game *g, Regs *r);   /* $2159ec */

/* ---- car model ---- */
void car_update(Game *g, uint32_t view);       /* $2129f2 */
void car_checkpoint(Game *g, uint32_t view);   /* $212680 */
void car_clock(Game *g, uint32_t view);        /* $21263c */
void car_distance(Game *g, uint32_t view, uint32_t *regs); /* $212662; D0,D1 */
void car_shape(Game *g, uint32_t view);        /* $212ba4 */
void car_update_regs(Game *g, Regs *r);        /* $2129f2, registers too */
void car_shape_regs(Game *g, Regs *r);         /* $212ba4, registers too */
void car_checkpoint_regs(Game *g, Regs *r);    /* $212680, registers too */
uint32_t sfx_claim_voice(Game *g, uint32_t d0); /* $20d7e8; returns D0 */
void car_drive(Game *g, uint32_t view);        /* $212734 */
void car_tick(Game *g, uint32_t view);         /* $21270a */
uint32_t car_frame_latch(Game *g);             /* $211dd4; returns A1 */
void race_frame_begin(Game *g, uint16_t d0_in); /* $212cea */
void race_frame_publish(Game *g);              /* $212e58 */
void car_latch_gap(Game *g);                   /* glue: $211058 only */

/* ---- input ---- */
void input_read(Game *g, const Input *in);     /* $211770 */

uint16_t road_bands(Game *g, Blitter *bl, uint32_t a0, uint32_t a4,
                    uint32_t a2, uint16_t d1, uint16_t d2, uint16_t d4,
                    uint32_t d5, uint16_t d6);   /* $213534 */

/* ---- per-frame display verbs (VERBS.md §2, ported from decomp.c) ---- */
void swap_screens(Game *g);                          /* $20f69e */
void build_fade(Game *g);                            /* $2102ca */
void build_copper_planes(Game *g, int planes, uint32_t buf); /* $210296 */
void load_palette(Game *g);                          /* $210272 */

#endif
