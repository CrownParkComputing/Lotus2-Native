/* blitter.h -- native Amiga blitter model for the Lotus 2 engine.
 *
 * The game draws almost everything with the blitter, so a native port
 * needs its own model rather than borrowing the emulator's.  This is a
 * direct port of the host's implementation (src/host/amiga.c blit()),
 * kept feature-complete -- all four channels, minterms, both barrel
 * shifters, first/last word masks, per-channel modulos and descending
 * mode -- because the road only uses shifted A->D copies but the rest of
 * the game does not.
 */
#ifndef LOTUS2_BLITTER_H
#define LOTUS2_BLITTER_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t *chip;          /* chip RAM the blitter reads and writes */
    uint32_t chip_size;     /* wraps at this size, like the real bus */
    uint16_t bltcon0, bltcon1;
    uint16_t bltafwm, bltalwm;
    uint32_t bltpt[4];      /* A, B, C, D */
    int16_t  bltmod[4];
    uint16_t bltdat[3];     /* A, B, C source data when a channel is off */
    bool     blt_zero;      /* set when every word written was zero */
    long     blits;         /* count, for parity against the oracle */
} Blitter;

/* Run one blit.  `size` is the BLTSIZE word: height in bits 15..6,
 * width in words in bits 5..0, with zero meaning the maximum. */
void blitter_run(Blitter *b, uint16_t size);

#endif
