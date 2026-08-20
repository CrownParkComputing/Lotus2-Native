/* engine.c -- ported Lotus 2 verbs (route B: semantic rewrite).
 *
 * Each function names one 68000 routine from re/VERBS.md; the source of
 * truth for the body is re/pipeline/disasm/decomp.c cross-checked against
 * the register trace.  Arithmetic widths are kept exact (PORTING_GUIDE).
 */
#include <stdio.h>
#include <stdlib.h>
#include "engine.h"

uint8_t *guest_load(const char *path, size_t expect, size_t *size_out)
{
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return NULL; }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0 || (expect && (size_t)size != expect)) {
        fprintf(stderr, "%s: size %ld, expected %zu\n", path, size, expect);
        fclose(file);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)size);
    if (buf && fread(buf, 1, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "%s: short read\n", path);
        free(buf);
        buf = NULL;
    }
    fclose(file);
    if (size_out) *size_out = (size_t)size;
    return buf;
}

/* $20f69e [verified]: exchange the two chip screen-buffer longs. */
void swap_screens(Game *g)
{
    uint32_t a = g32(g->base, G_SCREEN_A);
    uint32_t b = g32(g->base, G_SCREEN_B);
    p32(g->base, G_SCREEN_A, b);
    p32(g->base, G_SCREEN_B, a);
}

/* $2102ca [verified]: rebuild the fade-bank table at chip $5400 from the
 * 32-colour master palette at base $320c.  Nine banks, steps 0,2,..,$10;
 * per-nibble multiply then >>4, so step $10 is the identity. */
void build_fade(Game *g)
{
    uint32_t dst = CHIP_FADE_BANKS;
    for (int step = 0; step != 0x12; step += 2)
        for (int i = 0; i < 32; i++) {
            uint16_t c = g16(g->base, G_PALETTE + i * 2);
            uint16_t f = (uint16_t)((((c & 0x00f) * step) & 0x00f0) +
                                    (((c & 0x0f0) * step) & 0x0f00) +
                                    (((c & 0xf00) * step) & 0xf000)) >> 4;
            p16(g->chip, dst, f);
            dst += 2;
        }
}

/* $210296 [verified against decomp]: patch the copper list under
 * construction — BPLCON0 operand at $7ff22, then `planes` BPLxPTH/L
 * operand pairs from `buf`, plane stride $1f40.  NOTE: d0 is the plane
 * COUNT (the decomp loop runs d0 times); VERBS.md's "planes-1" reading
 * was wrong and is corrected here. */
void build_copper_planes(Game *g, int planes, uint32_t buf)
{
    p16(g->chip, CHIP_COP_BPLCON0, (uint16_t)((planes << 12) | 0x200));
    uint32_t at = CHIP_COP_BPLPTRS;
    for (int i = 0; i < planes; i++) {
        p16(g->chip, at + 2, (uint16_t)(buf >> 16));  /* BPLxPTH operand */
        p16(g->chip, at + 6, (uint16_t)buf);          /* BPLxPTL operand */
        at += 8;
        buf += LOTUS2_PLANE_STRIDE;
    }
}

/* $210272 [verified]: copy the current fade bank (selector at base $3000)
 * into the copper COLOR00..31 operands at $7ff74. */
void load_palette(Game *g)
{
    uint16_t bank = g16(g->base, G_PAL_BANK);
    uint32_t src = CHIP_FADE_BANKS + (uint32_t)(bank * 0x40);
    uint32_t dst = CHIP_COP_PALETTE;
    for (int i = 0; i < 32; i++) {
        p16(g->chip, dst + 2, g16(g->chip, src));
        src += 2;
        dst += 4;
    }
}
