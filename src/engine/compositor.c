/* compositor.c -- native display compositor for the Lotus 2 engine.
 *
 * A per-line copper interpreter with the SAME timing model as the oracle
 * host (src/host/amiga.c), so the PARITY gate can demand byte equality
 * against oracle screenshots:
 *
 *   - each raster line snapshots the bitplane pointers, THEN runs the
 *     copper for that line, THEN renders: pointer writes land on the NEXT
 *     line, but COLOUR writes behind an hp=0 WAIT backdate to the start
 *     of the current line (the host's color_line_start[] update in
 *     custom_write), and plane depth/modulos are read live;
 *   - WAIT compares (line & 0xff) & mask >= target & mask and re-arms with
 *     the beam-wrap bit, so lists that ride the 256-line wrap work;
 *   - COP1LC/COP2LC/COPJMPx are honoured; a jump pauses the copper until
 *     the next line (host copper_jump1/2 semantics);
 *   - bitplane pointers advance by fetch + modulo per displayed line,
 *     unless the copper re-pointed them during that line.
 *
 * Colour math matches the host's rgb4()/write_ppm() byte-for-byte.
 * Not modelled yet: sprites, fine scroll (BPLCON1), dual playfield,
 * mid-line colour splits (WAITs with hp != 0), EHB/HAM -- nothing the
 * attract or racing screens have needed so far; extend when a gate frame
 * demands it.
 */
#include <stdio.h>
#include <string.h>
#include "engine.h"
#include "compositor.h"

#define COP_PLANES 6
#define FETCH_BYTES 40          /* lores 320-wide */
#define DISPLAY_LINES 313       /* PAL */

typedef struct {
    const uint8_t *chip;
    uint16_t color[32];
    uint32_t bplpt[COP_PLANES];
    uint16_t bplcon0, diwstrt, diwstop;
    int16_t bpl1mod, bpl2mod;
    uint32_t cop1lc, cop2lc;
    uint32_t pc;
    int wait_line;              /* -1 = copper stopped */
    int cur_line;
    bool stop;                  /* set by COPJMP: yield until next line */
    long moves;
} Cop;

static void cop_write(Cop *c, uint16_t reg, uint16_t data)
{
    if (reg >= 0x180 && reg < 0x1c0)
        c->color[(reg - 0x180) >> 1] = data;
    else if (reg >= 0x0e0 && reg < 0x0e0 + COP_PLANES * 4) {
        int plane = (reg - 0x0e0) >> 2;
        if (reg & 2)
            c->bplpt[plane] = (c->bplpt[plane] & 0xffff0000u) | data;
        else
            c->bplpt[plane] = (c->bplpt[plane] & 0xffffu) |
                              ((uint32_t)data << 16);
    }
    else if (reg == 0x100) c->bplcon0 = data;
    else if (reg == 0x108) c->bpl1mod = (int16_t)data;
    else if (reg == 0x10a) c->bpl2mod = (int16_t)data;
    else if (reg == 0x08e) c->diwstrt = data;
    else if (reg == 0x090) c->diwstop = data;
    else if (reg == 0x080) c->cop1lc = (c->cop1lc & 0xffff) | ((uint32_t)data << 16);
    else if (reg == 0x082) c->cop1lc = (c->cop1lc & 0xffff0000u) | (data & 0xfffe);
    else if (reg == 0x084) c->cop2lc = (c->cop2lc & 0xffff) | ((uint32_t)data << 16);
    else if (reg == 0x086) c->cop2lc = (c->cop2lc & 0xffff0000u) | (data & 0xfffe);
    else if (reg == 0x088) {
        c->pc = c->cop1lc & (GUEST_CHIP_SIZE - 1);
        c->wait_line = c->cur_line + 1;
        c->stop = true;
    }
    else if (reg == 0x08a) {
        c->pc = c->cop2lc & (GUEST_CHIP_SIZE - 1);
        c->wait_line = c->cur_line + 1;
        c->stop = true;
    }
    /* everything else (blitter, sprites, audio) is inert in a still frame */
}

static void cop_run_line(Cop *c, int line)
{
    if (c->wait_line < 0 || line < c->wait_line) return;
    c->stop = false;
    for (int guard = 0; guard < 4096; guard++) {
        uint16_t first = g16(c->chip, c->pc);
        uint16_t second = g16(c->chip, c->pc + 2);
        if (!(first & 1)) {                                   /* MOVE */
            c->pc = (c->pc + 4) & (GUEST_CHIP_SIZE - 1);
            cop_write(c, first & 0x01fe, second);
            if (c->stop) return;
            c->moves++;
        } else if (!(second & 1)) {                           /* WAIT */
            if (first == 0xffff && second == 0xfffe) {
                c->wait_line = -1;
                return;
            }
            int target = (first >> 8) & 0xff;
            int mask = ((second >> 8) & 0x7f) | 0x80;
            if (((line & 0xff) & mask) >= (target & mask)) {
                c->pc = (c->pc + 4) & (GUEST_CHIP_SIZE - 1);
            } else {
                c->wait_line = (line & 0x100) | target;
                if (c->wait_line <= line) c->wait_line += 0x100;
                return;
            }
        } else {                                              /* SKIP */
            int target = (first >> 8) & 0xff;
            int mask = ((second >> 8) & 0x7f) | 0x80;
            bool reached = ((line & 0xff) & mask) >= (target & mask);
            c->pc = (c->pc + (reached ? 8 : 4)) & (GUEST_CHIP_SIZE - 1);
        }
    }
    c->wait_line = -1;
}

/* Host colour packing (src/host/amiga.c rgb4) reproduced exactly. */
static uint32_t rgb4(uint16_t value)
{
    uint32_t red = (value >> 8) & 15;
    uint32_t green = (value >> 4) & 15;
    uint32_t blue = value & 15;
    return 0xff000000u | (blue * 17 << 16) | (green * 17 << 8) | red * 17;
}

int composite(const uint8_t *chip, uint32_t coplc, uint32_t *out)
{
    Cop c;
    memset(&c, 0, sizeof c);
    c.chip = chip;
    c.pc = coplc & (GUEST_CHIP_SIZE - 1);
    c.wait_line = 0;
    c.diwstrt = 0x2c81;         /* textbook defaults until the list says */
    c.diwstop = 0xf4c1;

    int top = -1;               /* output line 0 = first displayed line */
    for (int line = 0; line < DISPLAY_LINES; line++) {
        c.cur_line = line;
        uint32_t render_bplpt[COP_PLANES];
        memcpy(render_bplpt, c.bplpt, sizeof render_bplpt);

        cop_run_line(&c, line);
        /* hp=0 colour writes backdate to this line's start: render with
         * the live palette (mid-line splits: add when a frame needs them) */
        const uint16_t *palette = c.color;

        int vstart = (c.diwstrt >> 8) & 0xff;
        int vstop = (c.diwstop >> 8) & 0xff;
        if (vstop <= vstart) vstop += 0x100;
        if (line < vstart || line >= vstop) continue;
        if (top < 0) top = line;
        int y = line - top;
        if (y >= LOTUS2_SCREEN_H) break;

        int depth = (c.bplcon0 >> 12) & 7;   /* live, like the host */
        if (depth > LOTUS2_MAX_PLANES) depth = LOTUS2_MAX_PLANES;
        uint32_t *row = out + (size_t)y * LOTUS2_SCREEN_W;
        for (int x = 0; x < LOTUS2_SCREEN_W; x++) {
            unsigned index = 0;
            for (int p = 0; p < depth; p++) {
                uint32_t at = (render_bplpt[p] + (unsigned)(x >> 3)) &
                              (GUEST_CHIP_SIZE - 1);
                if ((chip[at] >> (7 - (x & 7))) & 1)
                    index |= 1u << p;
            }
            row[x] = rgb4(palette[index & 31]);
        }
        for (int p = 0; p < depth; p++)
            if (c.bplpt[p] == render_bplpt[p])
                c.bplpt[p] = render_bplpt[p] + FETCH_BYTES +
                             (uint32_t)((p & 1) ? c.bpl2mod : c.bpl1mod);
    }
    return c.moves ? 0 : -1;
}

int write_ppm_native(const char *path, const uint32_t *fb, int w, int h)
{
    FILE *file = fopen(path, "wb");
    if (!file) { perror(path); return 1; }
    fprintf(file, "P6\n%d %d\n255\n", w, h);
    for (int pixel = 0; pixel < w * h; pixel++) {
        uint32_t value = fb[pixel];
        fputc((value >> 16) & 0xff, file);
        fputc((value >> 8) & 0xff, file);
        fputc(value & 0xff, file);
    }
    if (fclose(file)) { perror(path); return 1; }
    return 0;
}
