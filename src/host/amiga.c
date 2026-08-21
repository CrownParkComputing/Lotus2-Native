/* Native OCS host for the original SWIV program.
 *
 * The 68000 still executes the byte-exact game, loaded exactly as WHDLoad
 * loads it (see whdload.c).  This file is the machine underneath it: chip RAM,
 * the expansion RAM the slave asks for, beam position, interrupts, CIA-A/B,
 * two joystick ports, the keyboard serial line, the copper, six bitplanes,
 * sprites, the blitter and four-channel Paula mixing.
 *
 * It is a port of the equivalent host written for Battle Squadron, which is
 * why the chipset here is title-neutral: nothing in this file knows anything
 * about SWIV. */
#include "amiga.h"
#include "cpu.h"
#include "whdload.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

uint8_t chip[CHIP_SIZE];
/* WHDLoad's ExpMem: SWIV asks for $8B000 bytes of it and runs the whole game
 * out of it, so this is not an optional extra -- without a second RAM region
 * the slave's very first load has nowhere to go.  On a real machine this is
 * Zorro fast RAM; the DMA engines below cannot see it, which is correct. */
uint8_t fast[FAST_SIZE];
uint32_t framebuf[SCREEN_W * SCREEN_H];
uint8_t joy_state[2];
long swiv_frame_no;
long swiv_blit_count;
long swiv_disk_load_count;
long swiv_copper_moves;
long swiv_nonblack_pixels;
long swiv_audio_writes;

static bool stopped;
static int cur_line;
static uint16_t dmacon, intena, intreq;
static uint32_t cop1lc;
static uint32_t cop2lc;
static bool video_enabled;
static uint32_t bplpt[6], sprpt[8];
static uint16_t bplcon0, bplcon1, bplcon2;
static int16_t bpl1mod, bpl2mod;
static uint16_t color[32];
static uint16_t diwstrt = 0x2c81, diwstop = 0x2cc1;
static uint16_t ddfstrt = 0x0038, ddfstop = 0x00d0;
static uint32_t cop_pc;
static int cop_wait_line;
static bool in_copper;
static bool cop_stop;
static uint32_t render_bplpt[6];
/* Where in the line the copper currently is, in colour clocks: a WAIT has a
 * horizontal target as well as a vertical one, and a title that highlights
 * one of two words on the SAME line does it by changing a colour partway
 * across.  Ignoring that made both words take the last colour written --
 * which is why both commander names highlighted at once on Hybris' title. */
static int cop_h;
/* Where the framebuffer's left edge sits in raster coordinates.  Normally the
 * textbook lores origin ($81), but a window WIDER than the buffer (Hybris'
 * title screen opens 336 pixels at hpos 120) is centred on what it can show
 * instead of being pinned there and losing the right-hand side. */
static int display_left = 0x81;
static uint16_t color_line_start[32];
typedef struct { int x; uint8_t index; uint16_t value; } ColorChange;
static ColorChange color_changes[128];
static int color_change_count;
/* The playfield colour index for each column of the current scanline, used to
 * resolve sprite-to-playfield priority from BPLCON2. */
static uint8_t playfield_index[SCREEN_W];

static uint8_t kbd_queue[32];
static void kbd_trace(const char *what, unsigned v);
static int kbd_head, kbd_tail;
static uint8_t kbd_sdr;
static bool kbd_pending;
/* CIA-A's interrupt control register.  Only the serial port (the keyboard)
 * raises anything here, but the register still has to read as ZERO for the
 * other sources: SWIV's loader polls it and treats a set SP bit as "a rawkey
 * is waiting", so a lazy $FF made it read the debug key on the first poll and
 * quit before drawing a frame. */
#define ciaa_icr_flags ciaa.icr_flags
#define ciaa_icr_mask ciaa.icr_mask

typedef struct {
    uint32_t lc, lc_play, pos, nbytes_play;
    uint16_t lenlatch, period, volume;
    double fraction;
    bool on;
} AudioChannel;
static AudioChannel audio[4];

#define AUDIO_RATE 44100
#define AUDIO_RING_FRAMES 32768
static int16_t audio_ring[AUDIO_RING_FRAMES * 2];
static int audio_write_pos, audio_read_pos;
/* The frontend's audio callback is the sole consumer and the emulation
 * thread is the sole producer.  Keep the occupancy atomic so callbacks can
 * drain the ring without taking a lock around the 68000 core. */
static _Atomic int audio_fill;

static SwivPcHook pc_hook;

/* Sprites written straight to the registers, as opposed to fetched by DMA
 * from a sprite list.  Hybris draws its whole score panel this way: the
 * copper writes SPRxDATB/SPRxDATA then SPRxPOS, several times across a single
 * scanline, so one sprite paints several glyphs.  Nothing about that goes
 * through a sprite list, so a host that only walks SPRxPT shows no panel at
 * all. */
static uint16_t spr_pos[8], spr_ctl[8], spr_data[8], spr_datb[8];
/* Hardware arming: writing SPRxDATA arms a sprite, writing SPRxCTL disarms
 * it.  Hybris relies on both -- after its panel it clears SPR0CTL/SPR1CTL and
 * then rewrites SPRxPOS to hand the channels back to DMA, and painting those
 * writes put two phantom glyphs next to the icons on the top row. */
static bool spr_armed[8];
typedef struct {
    uint8_t  sprite;
    int      hstart;
    uint16_t data, datb;
} SpritePaint;
static SpritePaint spr_paint[128];
static int spr_paint_count;

static uint16_t bltcon0, bltcon1, bltafwm, bltalwm;
static uint32_t bltpt[4];             /* A, B, C, D */
static int16_t bltmod[4];
static uint16_t bltdat[3];            /* A, B, C */
static bool blt_zero = true;

static void copper_start(void);
static void copper_jump1(void);
static void copper_jump2(void);

/* The vertical origin of the framebuffer: normally the display window's own
 * first line, but sprites are NOT clipped to the window vertically, so a
 * title that hangs its status icons above the playfield needs the buffer to
 * start where the sprites do.  Hybris opens its window at line 39 and puts
 * the core and bomb icons at line 30; anchoring on the window alone sliced
 * nine rows off the top of them. */
static int display_top;
static bool display_top_valid;

/* Any caller that renders a line without running a whole frame -- the self
 * tests and the still-frame tools -- gets the plain window top. */
static int frame_top(void)
{
    return display_top_valid ? display_top : (int)((diwstrt >> 8) & 0xff);
}

static void audio_dma_update(uint16_t old_dmacon)
{
    for (int channel = 0; channel < 4; channel++) {
        bool was_on = (old_dmacon & 0x0200) &&
                      (old_dmacon & (1u << channel));
        bool is_on = (dmacon & 0x0200) &&
                     (dmacon & (1u << channel));
        AudioChannel *state = &audio[channel];
        if (is_on && !was_on) {
            state->lc_play = state->lc;
            state->pos = 0;
            state->fraction = 0;
            state->nbytes_play = (uint32_t)state->lenlatch * 2;
            state->on = true;
        } else if (!is_on) {
            state->on = false;
        }
    }
}

/* The two 8520s' timers.  Both timers of each CIA are modelled: SWIV and
 * Battle Squadron only ever start CIA-B timer A, but Uridium 2 drives its
 * whole game tick from CIA-B timer B (its copper interrupt starts TB in
 * one-shot mode and the TB interrupt runs the frame), so a CIA with only
 * timer A leaves that title sitting in its idle loop for ever. */
typedef struct {
    uint16_t latch, count;
    bool on, oneshot;
} CiaTimer;
typedef struct {
    CiaTimer t[2];
    uint8_t icr_mask, icr_flags;
    int frac;
} Cia;
static Cia ciaa, ciab;

static uint16_t rw(uint32_t address)
{
    address &= CHIP_SIZE - 1;
    return ((uint16_t)chip[address] << 8) |
           chip[(address + 1) & (CHIP_SIZE - 1)];
}

static void ww(uint32_t address, uint16_t value)
{
    address &= CHIP_SIZE - 1;
    chip[address] = value >> 8;
    chip[(address + 1) & (CHIP_SIZE - 1)] = (uint8_t)value;
}

static uint32_t rl(uint32_t address)
{
    return ((uint32_t)rw(address) << 16) | rw(address + 2);
}

static void irq_update(void)
{
    static const uint8_t level[14] = {
        1, 1, 1, 2, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6
    };
    int active_level = 0;
    if (intena & 0x4000) {
        uint16_t active = intena & intreq & 0x3fff;
        for (int bit = 0; bit < 14; bit++)
            if ((active & (1u << bit)) && level[bit] > active_level)
                active_level = level[bit];
    }
    cpu_set_irq(active_level);
}

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

/* Every distinct blit source, for ripping graphics: a BOB is drawn from its
 * artwork, so logging where the blitter reads gives the address, width and
 * height of every piece of art the game actually puts on screen -- far more
 * reliable than guessing at the layout of a packed data file. */
static void log_blit_source(uint32_t source, int width, int height,
                            uint16_t con0, int modulo, uint32_t dest,
                            int dest_mod)
{
    static FILE *log;
    static struct { uint32_t source; int width, height; } seen[4096];
    static int count;
    const char *path = getenv("SWIV_DUMP_BLITS");
    if (!path) return;
    if (!source || width <= 0 || height <= 0) return;
    /* SWIV_DUMP_BLITS_ALL keeps every blit, which is what shows a FORMATION:
     * one source drawn several times in a single frame. */
    if (!getenv("SWIV_DUMP_BLITS_ALL")) {
        for (int i = 0; i < count; i++)
            if (seen[i].source == source && seen[i].width == width &&
                seen[i].height == height) return;
        if (count == (int)(sizeof seen / sizeof seen[0])) return;
        seen[count].source = source; seen[count].width = width;
        seen[count].height = height; count++;
    }
    if (!log) { log = fopen(path, "w"); if (!log) return; }
    fprintf(log, "%06x %d %d %04x %d %06x %d %ld %06x\n", source, width,
            height, con0, modulo, dest, dest_mod, swiv_frame_no, bplpt[0]);
    fflush(log);
}

/* Blit sources a title has claimed, and the draw requests they produce. */
typedef struct { uint32_t low, high; int id; } Replacement;
static Replacement replacements[16];
static int replacement_count;
/* Identify mode: record where every claimed source lands but let the original
 * draw anyway, so a frame can be matched against the addresses that drew it. */
static bool replacements_suppress = true;
SwivSpriteDraw swiv_sprite_draws[64];
int swiv_sprite_draw_count;
static uint32_t frame_bpl0;          /* bitplane 0 at the top of the frame */

void amiga_register_replacement(uint32_t low, uint32_t high, int id)
{
    if (replacement_count == (int)(sizeof replacements / sizeof *replacements))
        return;
    replacements[replacement_count].low = low;
    replacements[replacement_count].high = high;
    replacements[replacement_count].id = id;
    replacement_count++;
}

void amiga_clear_replacements(void) { replacement_count = 0; }

void amiga_replacements_suppress(bool on) { replacements_suppress = on; }

/* Returns the id if this blit is a claimed one AND is the first plane of it,
 * having recorded where it would have landed; -1 otherwise. */
static int claim_blit(uint32_t source, uint32_t dest, int words, int rows)
{
    if (!replacement_count || !frame_bpl0) return -1;
    for (int i = 0; i < replacement_count; i++) {
        if (source < replacements[i].low || source >= replacements[i].high)
            continue;
        /* Only plane 0 produces a request; the other four land a bitplane
         * further on each and would otherwise draw the same thing five
         * times. */
        int32_t offset = (int32_t)(dest - frame_bpl0);
        if (offset < 0 || offset >= 0x4000) return replacements[i].id;
        if (swiv_sprite_draw_count <
            (int)(sizeof swiv_sprite_draws / sizeof *swiv_sprite_draws)) {
            /* The game draws into the buffer it is not displaying, so a
             * destination can be a whole screen further on than the pointer
             * being scanned out.  The buffer is 256 rows; fold it back. */
            int row = (offset / 32) % 256;
            int column = (offset % 32) * 8;
            SwivSpriteDraw *draw = &swiv_sprite_draws[swiv_sprite_draw_count++];
            draw->x = column + ((int)(diwstrt & 0xff) - display_left);
            draw->y = row + (((int)(diwstrt >> 8) & 0xff) - display_top);
            draw->width = words * 16;
            draw->height = rows;
            draw->id = replacements[i].id ? replacements[i].id
                                          : (int)source;
        }
        return replacements[i].id;
    }
    return -1;
}

static void blit(uint16_t size)
{
    int height = (size >> 6) & 0x3ff;
    int width = size & 0x3f;
    if (!height) height = 1024;
    if (!width) width = 64;
    /* A claimed source draws nothing: the frontend paints it instead. */
    bool claimed = ((bltcon0 & 0x0800) &&
                    claim_blit(bltpt[0], bltpt[3], width, height) >= 0) ||
                   ((bltcon0 & 0x0400) &&
                    claim_blit(bltpt[1], bltpt[3], width, height) >= 0);
    if (claimed && replacements_suppress) return;
    if (bltcon0 & 0x0800)
        log_blit_source(bltpt[0], width, height, bltcon0, bltmod[0],
                        bltpt[3], bltmod[3]);
    if (bltcon0 & 0x0400)
        log_blit_source(bltpt[1], width, height, bltcon0, bltmod[1],
                        bltpt[3], bltmod[3]);
    int ashift = (bltcon0 >> 12) & 15;
    int bshift = (bltcon1 >> 12) & 15;
    bool usea = bltcon0 & 0x0800;
    bool useb = bltcon0 & 0x0400;
    bool usec = bltcon0 & 0x0200;
    bool used = bltcon0 & 0x0100;
    bool descending = bltcon1 & 2;
    int step = descending ? -2 : 2;

    blt_zero = true;
    for (int y = 0; y < height; y++) {
        uint32_t aprevious = 0, bprevious = 0;
        for (int x = 0; x < width; x++) {
            uint16_t araw = usea ? rw(bltpt[0]) : bltdat[0];
            uint16_t braw = useb ? rw(bltpt[1]) : bltdat[1];
            uint16_t c = usec ? rw(bltpt[2]) : bltdat[2];
            if (x == 0) araw &= bltafwm;
            if (x == width - 1) araw &= bltalwm;
            uint16_t a, b;
            if (!descending) {
                a = (uint16_t)(((aprevious << 16) | araw) >> ashift);
                b = (uint16_t)(((bprevious << 16) | braw) >> bshift);
            } else {
                a = ashift ? (uint16_t)((((uint32_t)araw << 16) |
                                         aprevious) >> (16 - ashift)) : araw;
                b = bshift ? (uint16_t)((((uint32_t)braw << 16) |
                                         bprevious) >> (16 - bshift)) : braw;
            }
            aprevious = araw;
            bprevious = braw;
            uint16_t d = minterm(bltcon0 & 0xff, a, b, c);
            if (d) blt_zero = false;
            if (usea) bltpt[0] += step;
            if (useb) bltpt[1] += step;
            if (usec) bltpt[2] += step;
            if (used) {
                ww(bltpt[3], d);
                bltpt[3] += step;
            }
        }
        for (int channel = 0; channel < 4; channel++) {
            bool used_channel = channel == 0 ? usea : channel == 1 ? useb :
                                channel == 2 ? usec : used;
            if (used_channel)
                bltpt[channel] += descending ? -bltmod[channel]
                                             : bltmod[channel];
        }
    }
    swiv_blit_count++;
    intreq |= 0x0040;
    irq_update();
}

/* Which input registers a title actually polls, and from where: the only
 * reliable way to wire a button it reads through a path this host did not
 * expect. */
static void trace_input(const char *what, uint32_t value)
{
    if (!getenv("SWIV_TRACE_INPUT")) return;
    static uint32_t seen[64];
    static int count;
    uint32_t pc = cpu_get_reg(CPU_REG_PPC);
    for (int i = 0; i < count; i++)
        if (seen[i] == pc) return;
    if (count < 64) seen[count++] = pc;
    fprintf(stderr, "input: %s read from pc=$%06x (value $%04x)\n",
            what, pc, value);
}

static uint16_t custom_read(uint32_t reg)
{
    switch (reg) {
    case 0x002: {
        uint16_t value = dmacon & 0x07ff;
        if (blt_zero) value |= 0x2000;
        return value;
    }
    case 0x004: return (cur_line >> 8) & 7;
    case 0x006: return (cur_line & 0xff) << 8;
    case 0x00a:
    case 0x00c: {
        uint8_t state = joy_state[reg == 0x00a ? 0 : 1];
        trace_input(reg == 0x00a ? "JOY0DAT" : "JOY1DAT", state);
        int up = state & 1;
        int down = (state >> 1) & 1;
        int left = (state >> 2) & 1;
        int right = (state >> 3) & 1;
        return (uint16_t)((left << 9) | ((up ^ left) << 8) |
                          (right << 1) | (down ^ right));
    }
    case 0x016: {
        uint16_t value = 0xffff;       /* second buttons are active low */
        if (joy_state[0] & 0x20) value &= (uint16_t)~0x0004;
        if (joy_state[1] & 0x20) value &= (uint16_t)~0x0040;
        trace_input("POTGOR", value);
        return value;
    }
    case 0x012:
    case 0x014: {
        /* POTxDAT.  A second joystick button pulls pin 9 low, which stops
         * that pot line counting -- which is how a game reads a two-button
         * stick without POTGOR.  Hybris does exactly that: its input
         * aggregator sets the same bit for a POT0DAT change as it does for
         * SPACE. */
        uint8_t state = joy_state[reg == 0x012 ? 0 : 1];
        uint8_t pin9 = (state & 0x20) ? 0x00 : 0xff;   /* second button */
        uint8_t pin5 = (state & 0x40) ? 0x00 : 0xff;   /* third button */
        trace_input(reg == 0x012 ? "POT0DAT" : "POT1DAT", pin9);
        return (uint16_t)((pin9 << 8) | pin5);
    }
    case 0x01c: return intena & 0x7fff;
    case 0x01e: return intreq & 0x7fff;
    default: return 0;
    }
}

static void setclr(uint16_t *reg, uint16_t value)
{
    if (value & 0x8000) *reg |= value & 0x7fff;
    else *reg &= (uint16_t)~(value & 0x7fff);
}

static void custom_write(uint32_t reg, uint16_t value)
{
    switch (reg) {
    case 0x040: bltcon0 = value; break;
    case 0x042: bltcon1 = value; break;
    case 0x044: bltafwm = value; break;
    case 0x046: bltalwm = value; break;
    case 0x048: bltpt[2] = (bltpt[2] & 0xffff) | ((uint32_t)value << 16); break;
    case 0x04a: bltpt[2] = (bltpt[2] & 0xffff0000) | (value & 0xfffe); break;
    case 0x04c: bltpt[1] = (bltpt[1] & 0xffff) | ((uint32_t)value << 16); break;
    case 0x04e: bltpt[1] = (bltpt[1] & 0xffff0000) | (value & 0xfffe); break;
    case 0x050: bltpt[0] = (bltpt[0] & 0xffff) | ((uint32_t)value << 16); break;
    case 0x052: bltpt[0] = (bltpt[0] & 0xffff0000) | (value & 0xfffe); break;
    case 0x054: bltpt[3] = (bltpt[3] & 0xffff) | ((uint32_t)value << 16); break;
    case 0x056: bltpt[3] = (bltpt[3] & 0xffff0000) | (value & 0xfffe); break;
    case 0x058: blit(value); break;
    case 0x060: bltmod[2] = (int16_t)value; break;
    case 0x062: bltmod[1] = (int16_t)value; break;
    case 0x064: bltmod[0] = (int16_t)value; break;
    case 0x066: bltmod[3] = (int16_t)value; break;
    case 0x070: bltdat[2] = value; break;
    case 0x072: bltdat[1] = value; break;
    case 0x074: bltdat[0] = value; break;
    case 0x080: cop1lc = (cop1lc & 0xffff) | ((uint32_t)value << 16); if (getenv("SWIV_TRACE_COP1")) fprintf(stderr, "COP1LCH=%04x pc=%06x frame %ld\n", value, cpu_get_reg(CPU_REG_PC), swiv_frame_no); break;
    case 0x082: cop1lc = (cop1lc & 0xffff0000) | (value & 0xfffe); if (getenv("SWIV_TRACE_COP1")) fprintf(stderr, "COP1LCL=%04x pc=%06x frame %ld\n", value, cpu_get_reg(CPU_REG_PC), swiv_frame_no); break;
    case 0x084: cop2lc = (cop2lc & 0xffff) | ((uint32_t)value << 16); break;
    case 0x086: cop2lc = (cop2lc & 0xffff0000) | (value & 0xfffe); break;
    case 0x088: copper_jump1(); break;
    case 0x08a: copper_jump2(); break;
    case 0x08e:
        if (getenv("SWIV_TRACE_DIW") && value != diwstrt)
            fprintf(stderr, "DIWSTRT $%04x (v %d..) line %d\n", value,
                    value >> 8, cur_line);
        diwstrt = value; break;
    case 0x090:
        if (getenv("SWIV_TRACE_DIW") && value != diwstop)
            fprintf(stderr, "DIWSTOP $%04x (v ..%d) line %d\n", value,
                    (value >> 8) | 0x100, cur_line);
        diwstop = value; break;
    case 0x092: ddfstrt = value; break;
    case 0x094: ddfstop = value; break;
    case 0x096: {
        uint16_t old_dmacon = dmacon;
        setclr(&dmacon, value);
        audio_dma_update(old_dmacon);
        break;
    }
    case 0x09a: setclr(&intena, value); irq_update(); break;
    case 0x09c: setclr(&intreq, value); irq_update(); break;
    case 0x100: bplcon0 = value; break;
    case 0x102: bplcon1 = value; break;
    case 0x104: bplcon2 = value; break;
    case 0x108: bpl1mod = (int16_t)value; break;
    case 0x10a: bpl2mod = (int16_t)value; break;
    default:
        if (reg >= 0x0e0 && reg < 0x0f8) {
            int plane = (reg - 0x0e0) / 4;
            if (reg & 2)
                bplpt[plane] = (bplpt[plane] & 0xffff0000) |
                                (value & 0xfffe);
            else
                bplpt[plane] = (bplpt[plane] & 0xffff) |
                                ((uint32_t)value << 16);
        } else if (reg >= 0x140 && reg < 0x180) {
            int sprite = (reg - 0x140) / 8;
            switch ((reg - 0x140) & 6) {
            case 0:
                spr_pos[sprite] = value;
                /* POS is written last in a copper-drawn glyph, so this is
                 * the point the pair is complete and can be painted. */
                if (getenv("SWIV_TRACE_SPRW") && spr_armed[sprite] &&
                    (spr_data[sprite] | spr_datb[sprite])) {
                    static long copper_writes, cpu_writes;
                    if (in_copper) copper_writes++; else cpu_writes++;
                    if (((copper_writes + cpu_writes) % 20000) == 0)
                        fprintf(stderr, "sprite POS paints: copper %ld, "
                                "cpu %ld\n", copper_writes, cpu_writes);
                }
                if (spr_armed[sprite] &&
                    (spr_data[sprite] | spr_datb[sprite]) &&
                    spr_paint_count < (int)(sizeof spr_paint /
                                            sizeof spr_paint[0])) {
                    SpritePaint *paint = &spr_paint[spr_paint_count++];
                    paint->sprite = (uint8_t)sprite;
                    paint->hstart = ((value & 0xff) << 1) |
                                    (spr_ctl[sprite] & 1);
                    paint->data = spr_data[sprite];
                    paint->datb = spr_datb[sprite];
                }
                break;
            case 2:
                spr_ctl[sprite] = value;
                spr_armed[sprite] = false;         /* CTL write disarms */
                break;
            case 4:
                spr_data[sprite] = value;
                spr_armed[sprite] = true;          /* DATA write arms */
                break;
            case 6: spr_datb[sprite] = value; break;
            }
        } else if (reg >= 0x120 && reg < 0x140) {
            int sprite = (reg - 0x120) / 4;
            if (reg & 2)
                sprpt[sprite] = (sprpt[sprite] & 0xffff0000) |
                                (value & 0xfffe);
            else
                sprpt[sprite] = (sprpt[sprite] & 0xffff) |
                                ((uint32_t)value << 16);
        } else if (reg >= 0x180 && reg < 0x1c0) {
            if (getenv("SWIV_TRACE_COL") && reg >= 0x1b0 && reg <= 0x1b6)
                fprintf(stderr, "line %3d %s COLOR%02d = $%03x pc=$%06x\n",
                        cur_line, in_copper ? "COPPER" : "cpu   ",
                        (reg - 0x180) / 2, value & 0xfff,
                        in_copper ? 0 : cpu_get_reg(CPU_REG_PC));
            {
                uint8_t index = (uint8_t)((reg - 0x180) / 2);
                uint16_t rgb = value & 0x0fff;
                if (getenv("SWIV_TRACE_COLW") &&
                    index == atoi(getenv("SWIV_TRACE_COLW")))
                    fprintf(stderr, "colw line %d COLOR%d=$%03x copper=%d "
                            "cop_h=%d\n", cur_line, index, rgb, in_copper,
                            cop_h);
                color[index] = rgb;
                /* A copper write partway across the line takes effect there;
                 * anything else applies from the start of the line. */
                if (in_copper && cop_h > 0 &&
                    color_change_count < (int)(sizeof color_changes /
                                               sizeof color_changes[0])) {
                    ColorChange *change = &color_changes[color_change_count++];
                    change->x = cop_h * 2 - display_left;
                    change->index = index;
                    change->value = rgb;
                } else {
                    color_line_start[index] = rgb;
                }
            }
        } else if (reg >= 0x0a0 && reg < 0x0e0) {
            AudioChannel *state = &audio[(reg - 0x0a0) / 16];
            swiv_audio_writes++;
            switch (reg & 15) {
            case 0: state->lc = (state->lc & 0xffff) |
                                ((uint32_t)value << 16); break;
            case 2: state->lc = (state->lc & 0xffff0000) |
                                (value & 0xfffe); break;
            case 4: state->lenlatch = value; break;
            case 6: state->period = value ? value : 1; break;
            case 8: state->volume = value & 0x7f; break;
            default: break;
            }
        }
        break;
    }
}

static void copper_start(void)
{
    if (getenv("SWIV_DUMP_COPPER") && swiv_frame_no == atol(getenv("SWIV_DUMP_COPPER"))) {
        uint32_t at = cop1lc & (CHIP_SIZE - 1);
        fprintf(stderr, "copper list at $%06x:\n", at);
        for (int i = 0; i < 2000; i++) {
            uint16_t a1 = rw(at), a2 = rw(at + 2);
            if (a1 & 1)
                fprintf(stderr, "  %s v=%d h=$%02x  ($%04x $%04x)\n",
                        (a2 & 1) ? "SKIP" : "WAIT", (a1 >> 8) & 0xff,
                        a1 & 0xfe, a1, a2);
            else
                fprintf(stderr, "  MOVE $%03x = $%04x\n", a1 & 0x1fe, a2);
            at += 4;
            if (a1 == 0xffff && a2 == 0xfffe) break;
        }
    }
    cop_pc = cop1lc & (CHIP_SIZE - 1);
    cop_wait_line = cop1lc ? 0 : -1;
}

void amiga_dump_copper(FILE *fp)
{
    uint32_t at = cop1lc & (CHIP_SIZE - 1);
    fprintf(fp, "cop1lc list at $%06x:\n", at);
    for (int i = 0; i < 2000; i++) {
        uint16_t a1 = rw(at), a2 = rw(at + 2);
        if (a1 & 1)
            fprintf(fp, "  %s v=%d h=$%02x  ($%04x $%04x)\n",
                    (a2 & 1) ? "SKIP" : "WAIT", (a1 >> 8) & 0xff,
                    a1 & 0xfe, a1, a2);
        else
            fprintf(fp, "  MOVE $%03x = $%04x\n", a1 & 0x1fe, a2);
        at += 4;
        if (a1 == 0xffff && a2 == 0xfffe) break;
    }
    at = cop2lc & (CHIP_SIZE - 1);
    fprintf(fp, "cop2lc list at $%06x:\n", at);
    for (int i = 0; i < 2000; i++) {
        uint16_t a1 = rw(at), a2 = rw(at + 2);
        if (a1 & 1)
            fprintf(fp, "  %s v=%d h=$%02x  ($%04x $%04x)\n",
                    (a2 & 1) ? "SKIP" : "WAIT", (a1 >> 8) & 0xff,
                    a1 & 0xfe, a1, a2);
        else
            fprintf(fp, "  MOVE $%03x = $%04x\n", a1 & 0x1fe, a2);
        at += 4;
        if (a1 == 0xffff && a2 == 0xfffe) break;
    }
}

static void copper_jump1(void)
{
    cop_pc = cop1lc & (CHIP_SIZE - 1);
    cop_wait_line = cur_line + 1;
    cop_stop = true;
}

static void copper_jump2(void)
{
    cop_pc = cop2lc & (CHIP_SIZE - 1);
    cop_wait_line = cur_line + 1;
    cop_stop = true;
}

static void copper_run_line(int line)
{
    if (cop_wait_line < 0 || line < cop_wait_line) return;
    cop_stop = false;
    for (int guard = 0; guard < 4096; guard++) {
        uint16_t first = rw(cop_pc);
        uint16_t second = rw(cop_pc + 2);
        if (!(first & 1)) {
            cop_pc = (cop_pc + 4) & (CHIP_SIZE - 1);
            in_copper = true;
            if (getenv("SWIV_TRACE_COPW")) {
                static long at_frame = -1;
                long from = getenv("SWIV_TRACE_COPW_FROM")
                    ? atol(getenv("SWIV_TRACE_COPW_FROM")) : 1500;
                if (at_frame != swiv_frame_no && swiv_frame_no > from) {
                    if (at_frame != swiv_frame_no && (first & 0x1fe) < 0x180)
                        fprintf(stderr, "cop line %3d reg $%03x = $%04x\n",
                                cur_line, first & 0x1fe, second);
                    if (cur_line >= 310) at_frame = swiv_frame_no;
                }
            }
            custom_write(first & 0x01fe, second);
            in_copper = false;
            if (cop_stop) return;
            swiv_copper_moves++;
        } else if (!(second & 1)) {
            if (first == 0xffff && second == 0xfffe) {
                cop_wait_line = -1;
                return;
            }
            int target = (first >> 8) & 0xff;
            int mask = ((second >> 8) & 0x7f) | 0x80;
            if (getenv("SWIV_TRACE_COP")) {
                static int shown;
                if (shown < 24) {
                    fprintf(stderr,
                        "line %3d WAIT first=$%04x second=$%04x target=%d "
                        "mask=$%02x -> %s\n", cur_line, first, second, target,
                        mask,
                        (((cur_line & 0xff) & mask) >= (target & mask))
                            ? "satisfied" : "stall");
                    shown++;
                }
            }
            if (((line & 0xff) & mask) >= (target & mask)) {
                cop_h = first & 0xfe;
                cop_pc = (cop_pc + 4) & (CHIP_SIZE - 1);
            } else {
                cop_wait_line = (line & 0x100) | target;
                if (cop_wait_line <= line) cop_wait_line += 0x100;
                return;
            }
        } else {
            /* SKIP: skip the next instruction once the beam has reached the
             * position.  Uridium 2's list keeps a `SKIP $0001,$ffff` (always
             * true) in front of its COPJMP2, so treating SKIP as a no-op sent
             * the copper through the second list, whose COPJMP1 landed on the
             * COP1LC=0 the list itself had just written -- the copper then
             * ran the exception vectors and the display never came back. */
            int target = (first >> 8) & 0xff;
            int htarget = first & 0xfe;
            int mask = ((second >> 8) & 0x7f) | 0x80;
            int hmask = second & 0xfe;
            int beam_v = (line & 0xff) & mask, want_v = target & mask;
            bool reached = beam_v > want_v ||
                (beam_v == want_v && (cop_h & hmask) >= (htarget & hmask));
            cop_pc = (cop_pc + (reached ? 8 : 4)) & (CHIP_SIZE - 1);
        }
    }
    cop_wait_line = -1;
}

static uint32_t rgb4(uint16_t value)
{
    uint32_t red = (value >> 8) & 15;
    uint32_t green = (value >> 4) & 15;
    uint32_t blue = value & 15;
    return 0xff000000u | (blue * 17 << 16) | (green * 17 << 8) | red * 17;
}

static int display_vstop(void)
{
    int start = (diwstrt >> 8) & 0xff;
    int stop = (diwstop >> 8) & 0xff;
    if (stop <= start) stop += 0x100;
    return stop;
}

/* The horizontal position a display window of DIWSTRT $81 starts at: the
 * textbook lores origin, and where a 320-wide window begins.  A window that
 * starts later than this is genuinely further right on the screen, so it has
 * to be drawn there -- Hybris opens a 255-pixel window at $A1 and belongs 32
 * pixels in from the left border, not flush against it. */
/* The first visible column of the PAL raster.  $70 rather than the textbook
 * $81 because the buffer is now wide enough for overscan, and every window
 * these titles open (Hybris at $78 and $A1, Battle Squadron at $80/$81) has
 * to land inside it at its true position. */
#define DISPLAY_ORIGIN_X 0x70
/* The first line of the visible PAL raster.  The vertical origin is FIXED,
 * exactly like the horizontal one: a window opened at line 64 genuinely sits
 * 20 lines lower on screen than one opened at 44, and centring each window
 * would throw that away.  Deriving it from anything that moves (sprite
 * positions, latched or not) makes the whole picture jump. */
#define DISPLAY_ORIGIN_Y 26

/* A calibration nudge, in lores pixels, applied to where the playfield is
 * sampled relative to the sprites.  The DDF-to-window relationship this host
 * derives is right for Battle Squadron; leaving it adjustable is how a title
 * that pairs DDFSTRT and DIWSTRT differently gets checked against the real
 * thing without a rebuild. */
int swiv_playfield_shift;

/* HIRES (BPLCON0 bit 15) fetches twice as many words per line and paints
 * them at half the width.  Hybris' credit scroller is a 2-plane hires screen;
 * laying its data out as lores made the text run off both edges and overlap
 * itself, because each line consumed half the bytes it should have. */
static bool hires_mode(void) { return (bplcon0 & 0x8000) != 0; }

static int fetch_bytes(void)
{
    int start = ddfstrt & 0xfc;
    int stop = ddfstop & 0xfc;
    int words;
    if (stop < start) words = hires_mode() ? 40 : 20;
    else if (hires_mode()) words = ((stop - start) >> 2) + 2;
    else words = ((stop - start) >> 3) + 1;
    if (words < 1) words = 1;
    if (words > (hires_mode() ? 50 : 25)) words = hires_mode() ? 50 : 25;
    return words * 2;
}

static int playfield_bit(uint32_t pointer, int source_x)
{
    int byte = source_x >= 0 ? source_x / 8 : -((-source_x + 7) / 8);
    int bit_in_byte = source_x - byte * 8;
    return (chip[(pointer + (uint32_t)byte) & (CHIP_SIZE - 1)] >>
            (7 - bit_in_byte)) & 1;
}

static void render_line(int line)
{
    int vstart = (diwstrt >> 8) & 0xff;
    int y = line - frame_top();
    if (line < vstart || line >= display_vstop() || y < 0 || y >= SCREEN_H)
        return;
    if (getenv("SWIV_TRACE_BPLPT") && line == 76 && (swiv_frame_no % 10) == 0)
        fprintf(stderr, "bplpt frame%5ld line%3d: [%06x %06x %06x %06x] mod=[%04x %04x] con1=%04x\n",
                swiv_frame_no, line, bplpt[0], bplpt[1], bplpt[2], bplpt[3],
                bpl1mod, bpl2mod, bplcon1);

    if (getenv("SWIV_TRACE_SPLIT") && color_change_count &&
        line == atoi(getenv("SWIV_TRACE_SPLIT"))) {
        fprintf(stderr, "line %d: %d colour change(s):", line,
                color_change_count);
        for (int i = 0; i < color_change_count; i++)
            fprintf(stderr, " [x=%d COLOR%d=$%03x]", color_changes[i].x,
                    color_changes[i].index, color_changes[i].value);
        fprintf(stderr, "\n");
    }
    uint32_t *output = &framebuf[y * SCREEN_W];
    /* A caller that renders a line without running a frame (the self tests,
     * the still-frame tools) has no line-start snapshot, so it just uses the
     * live palette. */
    uint16_t palette[32];
    memcpy(palette, display_top_valid ? color_line_start : color,
           sizeof palette);
    int next_change = display_top_valid ? 0 : color_change_count;
    int depth = (bplcon0 >> 12) & 7;
    int bytes = fetch_bytes();
    /* A scrolling playfield starts DMA one fetch slot early (usually $30
     * instead of $38), supplying the word that shifts into the left edge.
     * Position fetched data relative to the normal display origin; merely
     * counting the extra word but drawing it at x=0 causes a 16-pixel jump
     * whenever the game rolls its fine scroll back to the next coarse word. */
    /* Only a DDFSTRT EARLIER than the textbook $38 is an early fetch.  A
     * LATER one does not mean a negative lead -- how far the window opens
     * past the first fetched pixel is already what diw_bias measures, so
     * letting this go negative counted the same distance twice.  Hybris
     * (DDFSTRT $48, DIWSTRT_H $A1) was the case that exposed it: its
     * playfield, and every blitter object drawn into it, sat 32 pixels right
     * of the sprites, so the ship never lined up with its two pods. */
    int fetch_lead = (0x38 - (ddfstrt & 0xfc)) * 2;
    if (fetch_lead < 0) fetch_lead = 0;
    /* The playfield was positioned from the DDF fetch start while sprites are
     * positioned from DIWSTRT, so the two only agreed when the game used the
     * textbook pairing DIWSTRT_H = DDFSTRT*2 + 17.  Battle Squadron does not:
     * DDFSTRT $38 implies $81, but it sets DIWSTRT_H $90, moving the window 15
     * lores pixels right of where the fetched data begins.  Without this the
     * whole playfield sat 15 pixels left of every sprite, which is why overlay
     * panels drifted against the map as it scrolled. */
    int window_start = (diwstrt & 0xff);
    int window_stop = (int)(diwstop & 0xff) | 0x100;
    int diw_bias = window_start - ((int)(ddfstrt & 0xfc) * 2 + 17);
    /* The window cannot begin before the data does.  Hybris' title opens
     * nine pixels ahead of where this reckons the first fetched pixel lands,
     * and reading those columns walks backwards off the start of the line and
     * shows the tail of the line above -- content wrapping from the right
     * edge round to the left.  Blanking them instead just clips the sides, so
     * treat the data as starting where the window does. */
    if (fetch_lead + diw_bias < 0) diw_bias = -fetch_lead;
    int left = window_start - display_left;
    int visible = window_stop - window_start;
    if (visible < 0) visible = 0;
    /* A window can be WIDER than the data DDF fetches for it -- Hybris opens
     * 336 pixels but fetches 320.  Drawing the difference read off the end of
     * the line and into the next one, which showed up as the title graphics
     * overlapping themselves.  Past the fetched data the hardware has nothing
     * to show, so the border colour it is. */
    int fetched = fetch_bytes() * 8 / (hires_mode() ? 2 : 1);
    if (visible > fetched) visible = fetched;
    if (depth > 6) depth = 6;
    bool dma = (dmacon & 0x0300) == 0x0300;
    if (!dma || !depth) {
        for (int x = 0; x < SCREEN_W; x++) {
            while (next_change < color_change_count &&
                   color_changes[next_change].x <= x) {
                palette[color_changes[next_change].index] =
                    color_changes[next_change].value;
                next_change++;
            }
            playfield_index[x] = 0;
            output[x] = rgb4(palette[0]);
        }
        return;
    }

    bool dual = (bplcon0 & 0x0400) != 0;
    bool pf2_priority = (bplcon2 & 0x0040) != 0;
    for (int x = 0; x < SCREEN_W; x++) {
        while (next_change < color_change_count &&
               color_changes[next_change].x <= x) {
            palette[color_changes[next_change].index] =
                color_changes[next_change].value;
            next_change++;
        }
        /* Outside the display window the hardware shows the border colour,
         * on BOTH sides: a narrow window has a left border too. */
        if (x < left || x >= left + visible) {
            playfield_index[x] = 0;
            output[x] = rgb4(palette[0]);
            continue;
        }
        int window_x = x - left;
        /* In hires one buffer column covers two source pixels, so the
         * picture comes out the physical width it really is. */
        int source_scale = hires_mode() ? 2 : 1;
        int index = 0;
        if (!dual) {
            int source_x = (window_x + fetch_lead + diw_bias +
                            swiv_playfield_shift) * source_scale -
                           (bplcon1 & 15);
            for (int plane = 0; plane < depth; plane++)
                index |= playfield_bit(render_bplpt[plane], source_x) << plane;
        } else {
            int pf1 = 0, pf2 = 0;
            for (int plane = 0; plane < depth; plane++) {
                int scroll = (plane & 1) ? ((bplcon1 >> 4) & 15)
                                         : (bplcon1 & 15);
                int source_x = (window_x + fetch_lead + diw_bias +
                                swiv_playfield_shift) * source_scale - scroll;
                int value = playfield_bit(render_bplpt[plane], source_x);
                if (plane & 1) pf2 |= value << (plane >> 1);
                else pf1 |= value << (plane >> 1);
            }
            if (pf2_priority)
                index = pf2 ? 8 + pf2 : pf1;
            else
                index = pf1 ? pf1 : (pf2 ? 8 + pf2 : 0);
        }
        playfield_index[x] = (uint8_t)(index & 0xff);
        output[x] = rgb4(palette[index & 31]);
        if (index) swiv_nonblack_pixels++;
    }
    for (int plane = 0; plane < depth; plane++) {
        int inc = bytes + ((plane & 1) ? bpl2mod : bpl1mod);
        if (bplpt[plane] == render_bplpt[plane])
            bplpt[plane] = render_bplpt[plane] + inc;
    }
}

typedef struct {
    bool active, attached;
    int hstart;
    uint16_t low, high;
} SpriteLine;

static SpriteLine sprite_line(int number, int line)
{
    SpriteLine result = {0};
    uint32_t pointer = sprpt[number] & (CHIP_SIZE - 1);
    if (!pointer) return result;
    for (int guard = 0; guard < 64; guard++) {
        uint16_t pos = rw(pointer);
        uint16_t ctl = rw(pointer + 2);
        if (!pos && !ctl) break;
        int vstart = (pos >> 8) | ((ctl & 4) << 6);
        int vstop = (ctl >> 8) | ((ctl & 2) << 7);
        int rows = vstop - vstart;
        if (rows <= 0 || rows > 300) break;
        if (getenv("SWIV_TRACE_SPRV")) {
            static int reported[8][8];
            static long at_frame = -1;
            if (at_frame != swiv_frame_no) { memset(reported, 0, sizeof reported);
                                           at_frame = swiv_frame_no; }
            if (guard < 8 && !reported[number][guard]) {
                reported[number][guard] = 1;
                fprintf(stderr, "sprv%d image %d: lines %d..%d "
                        "(window starts %d)\n", number, guard, vstart, vstop,
                        (diwstrt >> 8) & 0xff);
            }
        }
        if (line >= vstart && line < vstop) {
            uint32_t data = pointer + 4 + (uint32_t)(line - vstart) * 4;
            result.active = true;
            result.attached = (number & 1) && (ctl & 0x0080);
            result.hstart = ((pos & 0xff) << 1) | (ctl & 1);
            result.low = rw(data);
            result.high = rw(data + 2);
            return result;
        }
        pointer = (pointer + 4 + (uint32_t)rows * 4) &
                  (CHIP_SIZE - 1);
    }
    return result;
}

/* Paint the glyphs queued by this line's register writes.  They are not
 * gated on sprite DMA: writing SPRxDATA displays a sprite whether or not the
 * DMA channel is running. */
static void paint_written_sprites(int line)
{
    if (getenv("SWIV_NO_WRITTEN_SPRITES")) { spr_paint_count = 0; return; }
    int y = line - frame_top();
    if (y < 0 || y >= SCREEN_H) { spr_paint_count = 0; return; }
    for (int i = 0; i < spr_paint_count; i++) {
        const SpritePaint *paint = &spr_paint[i];
        int bank = 16 + (paint->sprite / 2) * 4;
        for (int bit = 0; bit < 16; bit++) {
            int x = paint->hstart - display_left + bit;
            if (x < 0 || x >= SCREEN_W) continue;
            int index = ((paint->data >> (15 - bit)) & 1) |
                        (((paint->datb >> (15 - bit)) & 1) << 1);
            if (!index) continue;
            framebuf[y * SCREEN_W + x] = rgb4(color[bank + index]);
            swiv_nonblack_pixels++;
        }
    }
    spr_paint_count = 0;
}

static void render_sprites_line(int line)
{
    int y = line - frame_top();
    if (y < 0 || y >= SCREEN_H) return;
    /* Sprites are hidden past DIWSTOP just as the playfield is.  Clipping only
     * the playfield left sprite-drawn text stacked up in the right-hand border
     * -- the credits and message screens draw their glyphs as sprites. */
    /* Sprites are positioned in the same screen coordinates as the window,
     * so they use the same origin; clipping still follows DIWSTOP. */
    int sprite_left = (int)(diwstrt & 0xff) - display_left;
    int sprite_visible = (((int)(diwstop & 0xff) | 0x100) - (diwstrt & 0xff));
    if (sprite_visible < 0) sprite_visible = 0;
    if (sprite_left < 0) { sprite_visible += sprite_left; sprite_left = 0; }
    if (sprite_visible > SCREEN_W - sprite_left)
        sprite_visible = SCREEN_W - sprite_left;
    bool dual = (bplcon0 & 0x0400) != 0;

    /* Sprite colours can change in the copper list.  Compose each scanline
     * while that line's palette is live rather than colouring the whole
     * sprite with the palette left at the end of the frame. */
    for (int pair = 3; pair >= 0; pair--) {
        uint8_t pixels[2][SCREEN_W] = {{0}};
        SpriteLine lines[2] = {
            sprite_line(pair * 2, line),
            sprite_line(pair * 2 + 1, line)
        };
        for (int which = 0; which < 2; which++) {
            if (!lines[which].active) continue;
            if (getenv("SWIV_TRACE_SPR") &&
                line == atoi(getenv("SWIV_TRACE_SPR")))
                fprintf(stderr, "spr%d bank=%d c=[%03x %03x %03x] hstart=%d diwstrt=$%04x diwstop=$%04x "
                        "ddfstrt=$%03x ddfstop=$%03x bplcon0=$%04x -> x=%d, "
                        "display width=%d\n",
                        pair * 2 + which, 16 + pair * 4,
                        color[16 + pair * 4 + 1], color[16 + pair * 4 + 2],
                        color[16 + pair * 4 + 3], lines[which].hstart, diwstrt,
                        diwstop, ddfstrt, ddfstop, bplcon0,
                        lines[which].hstart - (diwstrt & 0xff),
                        ((diwstop & 0xff) | 0x100) - (diwstrt & 0xff));
            for (int bit = 0; bit < 16; bit++) {
                int x = lines[which].hstart - display_left + bit;
                if (x < 0 || x >= SCREEN_W) continue;
                pixels[which][x] =
                    ((lines[which].low >> (15 - bit)) & 1) |
                    (((lines[which].high >> (15 - bit)) & 1) << 1);
            }
        }
        int bank = 16 + pair * 4;
        /* BPLCON2 places the single playfield among the four sprite pairs:
         * the PF1P bits (0-2) are for dual playfields; in non-dual mode the
         * playfield priority is in the PF2P bits (3-5). */
        int pf1p = dual ? (bplcon2 & 7) : ((bplcon2 >> 3) & 7);
        bool pf_front = pair >= pf1p;
        for (int x = sprite_left; x < sprite_left + sprite_visible; x++) {
            if (pf_front && playfield_index[x] != 0)
                continue;
            if (lines[1].attached) {
                int index = pixels[0][x] | (pixels[1][x] << 2);
                if (index) {
                    framebuf[y * SCREEN_W + x] = rgb4(color[16 + index]);
                    swiv_nonblack_pixels++;
                }
            } else {
                int index = pixels[1][x];
                if (index) {
                    framebuf[y * SCREEN_W + x] = rgb4(color[bank + index]);
                    swiv_nonblack_pixels++;
                }
                index = pixels[0][x];
                if (index) {
                    framebuf[y * SCREEN_W + x] = rgb4(color[bank + index]);
                    swiv_nonblack_pixels++;
                }
            }
        }
    }
}

static uint8_t cia_read(uint32_t address)
{
    unsigned reg = (address >> 8) & 15;
    if (address >= 0xbfe000) {
        if (reg == 0) {
            uint8_t value = 0xff;
            if (joy_state[0] & 0x10) value &= (uint8_t)~0x40;
            if (joy_state[1] & 0x10) value &= (uint8_t)~0x80;
            trace_input("CIAA_PRA (fire)", value);
            return value;
        }
        if (reg == 12) { trace_input("CIAA_SDR (keyboard)", kbd_sdr);
                         return kbd_sdr; }
        if (reg == 13) {
            uint8_t value = ciaa_icr_flags;
            if (value & ciaa_icr_mask) value |= 0x80;
            ciaa_icr_flags = 0;
            trace_input("CIAA_ICR", value);
            kbd_trace("read ICR", value);
            return value;
        }
        return 0xff;
    }
    if (reg == 13) {
        uint8_t value = ciab.icr_flags;
        if (value & ciab.icr_mask) value |= 0x80;
        ciab.icr_flags = 0;
        return value;
    }
    if (reg >= 4 && reg <= 7) {
        const CiaTimer *t = &ciab.t[(reg - 4) >> 1];
        return (reg & 1) ? (uint8_t)(t->count >> 8) : (uint8_t)t->count;
    }
    return 0xff;
}

/* A control-register write (CRA/CRB) or a timer high-byte write for one
 * timer of either CIA. */
static void cia_timer_control(CiaTimer *t, uint8_t value)
{
    t->oneshot = (value & 8) != 0;
    if (value & 0x10) t->count = t->latch;
    t->on = (value & 1) != 0;
}

static void cia_write(uint32_t address, uint8_t value)
{
    unsigned reg = (address >> 8) & 15;
    Cia *cia = address >= 0xbfe000 ? &ciaa : &ciab;
    if (address >= 0xbfe000) {
        if (reg == 12) {
            kbd_sdr = value;
            kbd_pending = false;
            return;
        } else if (reg == 14) {
            if (value & 0x40) kbd_pending = false;
            kbd_trace("write CRA", value);
        }
    }
    switch (reg) {
    case 4: case 6: {
        CiaTimer *t = &cia->t[(reg - 4) >> 1];
        t->latch = (t->latch & 0xff00) | value;
        break;
    }
    case 5: case 7: {
        CiaTimer *t = &cia->t[(reg - 4) >> 1];
        t->latch = (t->latch & 0x00ff) | ((uint16_t)value << 8);
        /* A high-byte write loads the counter, and in one-shot mode also
         * starts it. */
        if (!t->on) t->count = t->latch;
        if (t->oneshot) t->on = true;
        break;
    }
    case 13:
        if (value & 0x80) cia->icr_mask |= value & 0x7f;
        else cia->icr_mask &= (uint8_t)~(value & 0x7f);
        break;
    case 14: cia_timer_control(&cia->t[0], value); break;
    case 15: cia_timer_control(&cia->t[1], value); break;
    default: break;
    }
}

void amiga_key_event(uint8_t rawcode, bool up)
{
    int next = (kbd_tail + 1) % (int)sizeof kbd_queue;
    if (next == kbd_head) return;
    kbd_queue[kbd_tail] = rawcode | (up ? 0x80 : 0);
    kbd_tail = next;
}

long swiv_keys_delivered, swiv_keys_blocked, swiv_keys_resent;
static int kbd_pending_age;
static uint8_t kbd_last_code;
static int kbdtrace = -1;
static void kbd_trace(const char *what, unsigned v)
{
    if (kbdtrace < 0) kbdtrace = getenv("SWIV_TRACE_KBD") ? 1 : 0;
    if (!kbdtrace) return;
    static int n;
    if (n++ < 40)
        fprintf(stderr, "kbd[%2d] frame %5ld %-18s %02x  pending=%d "
                "icr=%02x mask=%02x intreq=%04x intena=%04x\n",
                n, swiv_frame_no, what, v, kbd_pending, ciaa_icr_flags,
                ciaa_icr_mask, intreq, intena);
}

/* True when the keyboard is ready for another code: nothing queued and
 * the game has acknowledged the last one.  An injector that ignores this
 * and types on a fixed schedule loses every letter after the first. */
bool amiga_kbd_idle(void)
{
    return !kbd_pending && kbd_head == kbd_tail;
}
/* The keyboard resends an unacknowledged code.
 *
 * This game's PORTS handler tests Timer A first and returns if it is set:
 *
 *     btst #$0,D0 ; bne $20f63e     <- never reaches the bit 3 test
 *
 * so a rawkey byte whose SP flag shares an ICR read with a timer
 * interrupt is dropped, and the ICR read has already cleared the flag.
 * A real Amiga keyboard copes because it waits for the handshake and
 * RETRANSMITS after about 143 ms if it does not come; the typist never
 * notices.  Modelling that is what makes typing reliable here, and it is
 * hardware behaviour rather than a workaround: gating delivery on a
 * quiet interrupt window also worked, but only under Musashi's timing --
 * the recompiled CPU collided anyway and the password stalled at one
 * letter again.  A fix that depends on which CPU is running is not a fix
 * when one of them is the oracle for the other.
 */
#define KBD_RESEND_FRAMES 7        /* ~143 ms at 50 Hz */
static void kbd_pump(void)
{
    if (kbd_head == kbd_tail) return;
    if (kbd_pending) { swiv_keys_blocked++; return; }
    /* Wait for an ICR window with nothing else pending.  The handler
     * tests Timer A first and returns, so a byte sharing that window is
     * dropped and the ICR read has already cleared its flag. */
    if (ciaa_icr_flags) { swiv_keys_blocked++; return; }
    swiv_keys_delivered++;
    uint8_t code = kbd_queue[kbd_head];
    kbd_head = (kbd_head + 1) % (int)sizeof kbd_queue;
    uint8_t inverted = (uint8_t)~code;
    kbd_sdr = (uint8_t)((inverted << 1) | (inverted >> 7));
    kbd_pending = true;
    kbd_pending_age = 0;
    kbd_last_code = code;
    ciaa_icr_flags |= 0x08;             /* SP: a rawkey byte has arrived */
    intreq |= 0x0008;
    irq_update();
    kbd_trace("deliver", code);
}

/* Called once per frame: re-assert an unacknowledged byte. */
static void kbd_age(void)
{
    if (!kbd_pending) return;
    if (++kbd_pending_age < KBD_RESEND_FRAMES) return;
    if (ciaa_icr_flags) return;         /* still noisy; wait for a gap */
    kbd_pending_age = 0;
    ciaa_icr_flags |= 0x08;
    intreq |= 0x0008;
    irq_update();
    swiv_keys_resent++;
    kbd_trace("resend", kbd_last_code);
}

#define PAULA_CLOCK 3546895.0
static double audio_filter_left, audio_filter_right;
static unsigned long long audio_energy;
static bool audio_transition_muted;

/* SWIV_WAV4=path: raw per-channel Paula output (4 x int16 per sample,
 * sample*volume, before the pan and the output filter) for measuring one
 * voice out of the mix. */
static FILE *wav4; static int wav4_state = -1;
static void audio_mix(int16_t *output, int frames)
{
    if (wav4_state < 0) { wav4 = getenv("SWIV_WAV4") ? fopen(getenv("SWIV_WAV4"), "wb") : NULL; wav4_state = wav4 ? 1 : 0; }
    for (int frame = 0; frame < frames; frame++) {
        int32_t left = 0, right = 0; int16_t per_ch[4] = { 0, 0, 0, 0 };
        for (int channel = 0; channel < 4; channel++) {
            AudioChannel *state = &audio[channel];
            if (!state->on || state->period < 8 || !state->nbytes_play)
                continue;
            state->fraction += PAULA_CLOCK /
                               ((double)state->period * AUDIO_RATE);
            while (state->fraction >= 1.0) {
                state->fraction -= 1.0;
                if (++state->pos >= state->nbytes_play) {
                    state->pos = 0;
                    state->lc_play = state->lc;
                    state->nbytes_play = (uint32_t)state->lenlatch * 2;
                    intreq |= (uint16_t)(0x0080 << channel);
                }
            }
            int8_t sample = (int8_t)chip[(state->lc_play + state->pos) &
                                         (CHIP_SIZE - 1)];
            if (audio_transition_muted) continue;
            int volume = state->volume > 64 ? 64 : state->volume;
            int32_t value = sample * volume;
            per_ch[channel] = (int16_t)value;
            if (channel == 0 || channel == 3) left += value;
            else right += value;
        }
        if (wav4) fwrite(per_ch, 2, 4, wav4);
        double mixed_left = left * 0.75 + right * 0.25;
        double mixed_right = right * 0.75 + left * 0.25;
        audio_filter_left += 0.45 * (mixed_left - audio_filter_left);
        audio_filter_right += 0.45 * (mixed_right - audio_filter_right);
        int32_t left_out = (int32_t)(audio_filter_left * 1.8);
        int32_t right_out = (int32_t)(audio_filter_right * 1.8);
        if (left_out > 32767) left_out = 32767;
        if (left_out < -32768) left_out = -32768;
        if (right_out > 32767) right_out = 32767;
        if (right_out < -32768) right_out = -32768;
        output[frame * 2] = (int16_t)left_out;
        output[frame * 2 + 1] = (int16_t)right_out;
        audio_energy += (unsigned)(left_out < 0 ? -left_out : left_out);
        audio_energy += (unsigned)(right_out < 0 ? -right_out : right_out);
    }
    irq_update();
}

int amiga_audio_fill(void)
{
    return atomic_load_explicit(&audio_fill, memory_order_acquire);
}

/* Mix exactly `frames` samples into the ring.
 *
 * Rate correction has to be done in samples, not in whole video frames.
 * The device consumes 44100 a second while the game produces 882 per
 * frame at 49.75 fps -- a deficit of a few hundred samples a second.
 * Correcting that by mixing an extra 882-sample frame overshoots by two
 * orders of magnitude, so the ring surges and starves by turns, and the
 * pitch wobbles with it.  That is the rasp.
 */
void amiga_audio_generate(int frames)
{
    enum { CHUNK = AUDIO_RATE / 50 };
    int16_t mixed[CHUNK * 2];
    while (frames > 0) {
        int n = frames < CHUNK ? frames : CHUNK;
        if (atomic_load_explicit(&audio_fill, memory_order_acquire) + n >
            AUDIO_RING_FRAMES) return;
        audio_mix(mixed, n);
        for (int frame = 0; frame < n; frame++) {
            audio_ring[audio_write_pos * 2] = mixed[frame * 2];
            audio_ring[audio_write_pos * 2 + 1] = mixed[frame * 2 + 1];
            audio_write_pos = (audio_write_pos + 1) % AUDIO_RING_FRAMES;
        }
        atomic_fetch_add_explicit(&audio_fill, n, memory_order_release);
        frames -= n;
    }
}

void amiga_audio_frame(void) { amiga_audio_generate(AUDIO_RATE / 50); }

int amiga_audio_pull(int16_t *output, int frames)
{
    int available = atomic_load_explicit(&audio_fill, memory_order_acquire);
    int copied = frames < available ? frames : available;
    for (int frame = 0; frame < copied; frame++) {
        output[frame * 2] = audio_ring[audio_read_pos * 2];
        output[frame * 2 + 1] = audio_ring[audio_read_pos * 2 + 1];
        audio_read_pos = (audio_read_pos + 1) % AUDIO_RING_FRAMES;
    }
    if (copied)
        atomic_fetch_sub_explicit(&audio_fill, copied, memory_order_release);
    for (int frame = copied; frame < frames; frame++) {
        output[frame * 2] = 0;
        output[frame * 2 + 1] = 0;
    }
    return copied;
}

/* Advance one CIA's timers by a scanline's worth of E-clock (1/10 of the
 * 7 MHz cycles).  A timer that underflows raises its ICR bit, and if the
 * mask allows it the CIA's interrupt line: PORTS (level 2) for CIA-A, EXTER
 * (level 6) for CIA-B.  The flags are left standing for the game's own
 * handler to read, which is how it acknowledges the CIA. */
static void cia_tick(Cia *cia, uint16_t intreq_bit)
{
    cia->frac += CYCLES_PER_LINE;
    int ticks_line = cia->frac / 10;
    cia->frac %= 10;
    for (int which = 0; which < 2; which++) {
        CiaTimer *t = &cia->t[which];
        if (!t->on) continue;
        int ticks = ticks_line;
        while (ticks > 0) {
            if (t->count > ticks) {
                t->count -= ticks;
                break;
            }
            ticks -= t->count;
            t->count = t->latch;
            cia->icr_flags |= (uint8_t)(1 << which);
            if (t->oneshot) {
                t->on = false;
                ticks = 0;
            }
            if (cia->icr_mask & (1 << which)) {
                intreq |= intreq_bit;
                irq_update();
            }
            if (!t->latch) break;
        }
    }
}

static void ciab_tick(void)
{
    cia_tick(&ciaa, 0x0008);
    cia_tick(&ciab, 0x2000);
}

/* The CPU's view of RAM: chip RAM from zero, then the expansion region.  A
 * pointer is returned so both the CPU and the loader use one decode; anything
 * outside RAM (custom registers, the CIAs, unpopulated space) returns NULL and
 * is handled by the caller. */
uint8_t *amiga_ram(uint32_t address, uint32_t length)
{
    if (address < CHIP_SIZE)
        return length <= CHIP_SIZE - address ? chip + address : NULL;
    if (address >= FAST_BASE && address < FAST_BASE + FAST_SIZE)
        return length <= FAST_BASE + FAST_SIZE - address
            ? fast + (address - FAST_BASE) : NULL;
    return NULL;
}

/* SWIV_WATCH=lo-hi[,lo-hi...]: report the first time each PC reads or
 * writes inside a range (data-flow discovery: "who reads the tile set?").
 * Env-gated and zero cost when unset. */
static struct { uint32_t lo, hi; } watch[8];
static int watch_count = -1;
static void watch_hit(uint32_t address, int is_write, int size)
{
    static uint32_t seen[4096]; static int seen_count;
    uint32_t pc = cpu_get_reg(CPU_REG_PPC);
    uint32_t key = pc | (is_write ? 0x80000000u : 0);
    for (int i = 0; i < seen_count; i++) if (seen[i] == key) return;
    if (seen_count < 4096) seen[seen_count++] = key;
    fprintf(stderr, "watch: %s%d $%06x by pc=$%06x frame %ld\n",
            is_write ? "write" : "read", size, address, pc, swiv_frame_no);
}
static inline void watch_check(uint32_t address, int is_write, int size)
{
    if (watch_count < 0) {
        watch_count = 0;
        const char *spec = getenv("SWIV_WATCH");
        while (spec && *spec && watch_count < 8) {
            unsigned lo, hi;
            if (sscanf(spec, "%x-%x", &lo, &hi) == 2) {
                watch[watch_count].lo = lo; watch[watch_count].hi = hi;
                watch_count++;
            }
            spec = strchr(spec, ',');
            if (spec) spec++;
        }
    }
    for (int i = 0; i < watch_count; i++)
        if (address >= watch[i].lo && address < watch[i].hi)
            watch_hit(address, is_write, size);
}

unsigned int m68k_read_memory_8(unsigned int address)
{
    address &= 0xffffff;
    if (watch_count) watch_check(address, 0, 8);
    const uint8_t *ram = amiga_ram(address, 1);
    if (ram) return *ram;
    if ((address & 0xfff000) == 0xdff000) {
        uint16_t value = custom_read(address & 0x1fe);
        return address & 1 ? value & 0xff : value >> 8;
    }
    if (address >= 0xbfd000 && address < 0xbff000)
        return cia_read(address);
    return 0;
}

unsigned int m68k_read_memory_16(unsigned int address)
{
    address &= 0xffffff;
    if (watch_count) watch_check(address, 0, 16);
    const uint8_t *ram = amiga_ram(address, 2);
    if (ram) return ((unsigned)ram[0] << 8) | ram[1];
    if ((address & 0xfff000) == 0xdff000)
        return custom_read(address & 0x1fe);
    return 0;
}

unsigned int m68k_read_memory_32(unsigned int address)
{
    return (m68k_read_memory_16(address) << 16) |
           m68k_read_memory_16(address + 2);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    address &= 0xffffff;
    if (watch_count) watch_check(address, 1, 8);
    uint8_t *ram = amiga_ram(address, 1);
    if (ram) {
        *ram = (uint8_t)value;
    } else if (address >= 0xbfd000 && address < 0xbff000) {
        cia_write(address, (uint8_t)value);
    } else if ((address & 0xfff000) == 0xdff000) {
        custom_write(address & 0x1fe, (value << 8) | (value & 0xff));
    }
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    address &= 0xffffff;
    if (watch_count) watch_check(address, 1, 16);
    uint8_t *ram = amiga_ram(address, 2);
    if (ram) {
        ram[0] = value >> 8;
        ram[1] = (uint8_t)value;
    } else if ((address & 0xfff000) == 0xdff000) {
        custom_write(address & 0x1fe, (uint16_t)value);
    }
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    m68k_write_memory_16(address, value >> 16);
    m68k_write_memory_16(address + 2, value);
}

unsigned int m68k_read_disassembler_8(unsigned int address)
{ return m68k_read_memory_8(address); }
unsigned int m68k_read_disassembler_16(unsigned int address)
{ return m68k_read_memory_16(address); }
unsigned int m68k_read_disassembler_32(unsigned int address)
{ return m68k_read_memory_32(address); }

/* A ring of recently executed addresses.  When the game or the slave aborts,
 * the reason alone says nothing about how it got there; this is what turns
 * "reason 5 at $200494" into a path through the loader. */
#define PC_HISTORY 64
static uint32_t pc_history[PC_HISTORY];
static unsigned pc_history_at;

void amiga_pc_history(void)
{
    fprintf(stderr, "native: last %d addresses executed:\n", PC_HISTORY);
    for (unsigned i = 0; i < PC_HISTORY; i++) {
        uint32_t pc = pc_history[(pc_history_at + i) % PC_HISTORY];
        if (!pc) continue;
        char text[128];
        cpu_disassemble(text, pc);
        fprintf(stderr, "  $%06x  %s\n", pc, text);
    }
}

/* Recompiler support (env-gated, zero cost when unset):
 *   SWIV_PCSET=path   on exit, write every distinct PC executed (text, hex)
 *   SWIV_STATELOG=path  binary per-instruction record before each
 *                     instruction: u32 pc, u32 d0-7, u32 a0-7, u32 sr
 *                     (17 + 1 = 18 words), capped by SWIV_STATELOG_MAX */
static uint8_t *pcset_bits;
static FILE *sfxlog;
static FILE *statelog;
static long statelog_left = -1;
static int recomp_trace_on = -1;   /* -1 unknown, 0 off, 1 on */

/* SWIV_SNAP_PCS=hex[,hex...]: dump guest RAM + registers whenever one of
 * the listed PCs executes.  SWIV_SNAP_FROM=frame arms it (default 0),
 * SWIV_SNAP_MAX=n caps the snapshot count (default 8), SWIV_SNAP_PREFIX
 * names the files: PREFIX<idx>_<pc>.regs / _chip.bin / _fast.bin.
 * Entry+RTS pairs of a routine give the input/output states a native
 * port must transform between (the TRANSLATE verification loop). */
#define SNAP_MAX_PCS 32
static uint32_t snap_pcs[SNAP_MAX_PCS];
static int snap_pc_count;
static long snap_from, snap_max = 8, snap_taken;
static const char *snap_prefix;
static void snapshot_take(unsigned int pc)
{
    char path[512];
    snprintf(path, sizeof path, "%s%ld_%06x.regs", snap_prefix,
             snap_taken, pc);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "frame %ld\npc %06x\n", swiv_frame_no, pc);
        for (int i = 0; i < 8; i++)
            fprintf(f, "d%d %08x\n", i,
                    cpu_get_reg(CPU_REG_D0 + i));
        for (int i = 0; i < 8; i++)
            fprintf(f, "a%d %08x\n", i,
                    cpu_get_reg(CPU_REG_A0 + i));
        fprintf(f, "sr %04x\n", cpu_get_reg(CPU_REG_SR));
        /* Custom-chip state a native port cannot recover from RAM: the
         * blitter's control words, masks, modulos and channel pointers,
         * plus the live palette.  Without these a native blit cannot be
         * gated byte-for-byte against the oracle. */
        fprintf(f, "bltcon0 %04x\nbltcon1 %04x\n", bltcon0, bltcon1);
        fprintf(f, "bltafwm %04x\nbltalwm %04x\n", bltafwm, bltalwm);
        for (int i = 0; i < 4; i++)
            fprintf(f, "bltpt%d %08x\n", i, bltpt[i]);
        for (int i = 0; i < 4; i++)
            fprintf(f, "bltmod%d %04x\n", i, (uint16_t)bltmod[i]);
        for (int i = 0; i < 3; i++)
            fprintf(f, "bltdat%d %04x\n", i, bltdat[i]);
        fprintf(f, "bltzero %d\n", blt_zero ? 1 : 0);
        fprintf(f, "dmacon %04x\nbplcon0 %04x\n", dmacon, bplcon0);
        /* Input as the game sees it: the synthesised JOYxDAT words and the
         * CIA-A PRA fire bits, so a native port of the pad decoder can be
         * fed exactly what the 68000 read. */
        for (int port = 0; port < 2; port++) {
            uint8_t st = joy_state[port];
            int up = st & 1, down = (st >> 1) & 1;
            int left = (st >> 2) & 1, right = (st >> 3) & 1;
            fprintf(f, "joy%ddat %04x\n", port,
                    (uint16_t)((left << 9) | ((up ^ left) << 8) |
                               (right << 1) | (down ^ right)));
        }
        {
            uint8_t pra = 0xff;
            if (joy_state[1] & 0x10) pra &= (uint8_t)~0x80;
            if (joy_state[0] & 0x10) pra &= (uint8_t)~0x40;
            fprintf(f, "ciapra %02x\n", pra);
        }
        for (int i = 0; i < 32; i++)
            fprintf(f, "color%02d %04x\n", i, color[i] & 0x0fff);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s%ld_%06x_chip.bin", snap_prefix,
             snap_taken, pc);
    f = fopen(path, "wb");
    if (f) { fwrite(chip, 1, CHIP_SIZE, f); fclose(f); }
    snprintf(path, sizeof path, "%s%ld_%06x_fast.bin", snap_prefix,
             snap_taken, pc);
    f = fopen(path, "wb");
    if (f) {   /* ExpMem exceeds the amiga_ram() fast window: go via the bus */
        for (uint32_t at = 0x200000u; at < 0x200000u + 0x890000u; at += 2) {
            unsigned w = m68k_read_memory_16(at);
            fputc(w >> 8, f);
            fputc(w & 0xff, f);
        }
        fclose(f);
    }
    fprintf(stderr, "amiga: snapshot %ld at pc=$%06x frame %ld -> %s*\n",
            snap_taken, pc, swiv_frame_no, snap_prefix);
    snap_taken++;
}

/* Record a pc in the executed-pc set WITHOUT the snapshot/statelog side
 * effects, for addresses the instruction hook never sees. */
static void recomp_trace_pc_only(unsigned int pc)
{
    if (pcset_bits && pc < (1u << 24))
        pcset_bits[pc >> 3] |= 1u << (pc & 7);
}

static void recomp_trace(unsigned int pc)
{
    if (recomp_trace_on < 0) {
        recomp_trace_on = (getenv("SWIV_PCSET") || getenv("SWIV_STATELOG") ||
                           getenv("SWIV_SNAP_PCS")) ? 1 : 0;
        if (getenv("SWIV_SNAP_PCS")) {
            const char *at = getenv("SWIV_SNAP_PCS");
            while (snap_pc_count < SNAP_MAX_PCS && *at) {
                snap_pcs[snap_pc_count++] = (uint32_t)strtoul(at, NULL, 16);
                at = strchr(at, ',');
                if (!at) break;
                at++;
            }
            snap_from = getenv("SWIV_SNAP_FROM")
                ? atol(getenv("SWIV_SNAP_FROM")) : 0;
            if (getenv("SWIV_SNAP_MAX"))
                snap_max = atol(getenv("SWIV_SNAP_MAX"));
            snap_prefix = getenv("SWIV_SNAP_PREFIX");
            if (!snap_prefix) snap_prefix = "snap_";
        }
        if (getenv("SWIV_PCSET")) pcset_bits = calloc(1u << 21, 1);
        if (getenv("SWIV_STATELOG")) {
            statelog = fopen(getenv("SWIV_STATELOG"), "wb");
            statelog_left = getenv("SWIV_STATELOG_MAX")
                ? atol(getenv("SWIV_STATELOG_MAX")) : 2000000;
        }
    }
    if (snap_pc_count && snap_taken < snap_max && swiv_frame_no >= snap_from)
        for (int i = 0; i < snap_pc_count; i++)
            if (snap_pcs[i] == pc) { snapshot_take(pc); break; }
    if (pcset_bits && pc < (1u << 24))
        pcset_bits[pc >> 3] |= 1u << (pc & 7);
    if (statelog && statelog_left > 0 &&
        swiv_frame_no >= (getenv("SWIV_STATELOG_FROM") ? atol(getenv("SWIV_STATELOG_FROM")) : 0)) {
        uint32_t rec[18];
        rec[0] = pc;
        for (int i = 0; i < 8; i++) rec[1 + i] = cpu_get_reg(CPU_REG_D0 + i);
        for (int i = 0; i < 8; i++) rec[9 + i] = cpu_get_reg(CPU_REG_A0 + i);
        rec[17] = cpu_get_reg(CPU_REG_SR);
        fwrite(rec, sizeof rec, 1, statelog);
        if (--statelog_left == 0) { fclose(statelog); statelog = NULL; }
    }
}
void swiv_cycles_flush(void);
void swiv_rtncyc_flush(void);

void swiv_recomp_trace_flush(void)
{
    swiv_cycles_flush();
    swiv_rtncyc_flush();
    if (sfxlog) { fclose(sfxlog); sfxlog = NULL; }
    if (pcset_bits) {
        FILE *f = fopen(getenv("SWIV_PCSET"), "w");
        if (f) {
            for (uint32_t pc = 0; pc < (1u << 24); pc += 2)
                if (pcset_bits[pc >> 3] & (1u << (pc & 7)))
                    fprintf(f, "%06x\n", pc);
            fclose(f);
        }
    }
    if (statelog) fclose(statelog);
}

/* SWIV_SFXLOG=path: log the sound driver.  Runtime addresses of the AMPROG
 * listing (re/amprog.asm in SWIV-Native) are listing + 16 in this region.
 * Lines: "F pc D0 D1 D2 A0" at every effect entry point (D0 = x arg) and at
 * the voice starter LAB_03BC (D0 = prio*4, D1/D2 = args, A0 = body, plus the
 * voice index taken), and "T F" at each driver tick (CIA-B INT6). */
static int sfxlog_state = -1;
static const unsigned sfx_entry_pcs[] = {
    0x21093c, 0x21095c, 0x210978, 0x2109d2, 0x210a18, 0x210a8a, 0x210ae6,
    0x210b4e, 0x210b66, 0x210bc4, 0x210c5e, 0x210cbe, 0x210d1e, 0x210d90,
    0x210df0, 0x210e58, 0x210ef4, 0x210f5a, 0x210faa, 0x210ff8, 0x211070,
    0x2110de, 0x21113e, 0x211156, 0x2111cc, 0x21125a, 0x2112d0, 0x211320,
    0x211334, 0 };
static void sfxlog_hook(unsigned int pc)
{
    if (sfxlog_state < 0) {
        const char *path = getenv("SWIV_SFXLOG");
        sfxlog = path ? fopen(path, "w") : NULL;
        sfxlog_state = sfxlog ? 1 : 0;
    }
    if (!sfxlog || pc < 0x2107dc + 16 || pc > 0x211340 + 16) return;
    if (pc == 0x2107dc + 16) { fprintf(sfxlog, "T %ld\n", swiv_frame_no); return; }
    if (pc == 0x2108ea + 16) {
        unsigned a1 = cpu_get_reg(CPU_REG_A1);
        unsigned base = m68k_read_memory_32(0x2016DC + 10786);
        fprintf(sfxlog, "S %ld %u %04x %04x %04x %06x\n", swiv_frame_no,
                (a1 - base) / 268, cpu_get_reg(CPU_REG_D0) & 0xffff,
                cpu_get_reg(CPU_REG_D1) & 0xffff,
                cpu_get_reg(CPU_REG_D2) & 0xffff,
                cpu_get_reg(CPU_REG_A0) - 16);
        return;
    }
    for (int i = 0; sfx_entry_pcs[i]; i++)
        if (pc == sfx_entry_pcs[i] + 16) {
            fprintf(sfxlog, "E %ld %06x %04x\n", swiv_frame_no, pc - 16,
                    cpu_get_reg(CPU_REG_D0) & 0xffff);
            return;
        }
}

/* SWIV_CYCLES=path: measure how many cycles each pc costs.
 *
 * A static recompilation has no notion of time, and inventing one from a
 * timing manual would be a guess about the very thing the reference
 * frames were produced with.  The oracle already knows exactly: sample
 * the cycle counter at each instruction and the delta is the cost of the
 * PREVIOUS one.  Conditional branches and dbra differ between taken and
 * untaken, so the mean over every execution is recorded.
 */
static int cyclog_state = -1;
static unsigned cyc_last_pc;
static int cyc_last;

/* Costs are keyed by EDGE (pc -> next pc), not by pc.
 *
 * A conditional branch costs a different number of cycles taken than not
 * taken, and a per-pc mean rounds that difference away.  Averaged over a
 * frame the error is well under one percent, which still accumulates
 * into a whole frame of drift over a few thousand frames and eventually
 * sends the game down a different path.  Keyed by edge, each outcome
 * carries its own measured cost and nothing is averaged.
 */
#define CYC_SLOTS (1u << 21)
static struct { uint64_t key; unsigned sum, cnt; } *cyc_tab;

static unsigned cyc_slot(uint64_t key)
{
    unsigned h = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 43) % CYC_SLOTS;
    for (unsigned i = 0; i < CYC_SLOTS; i++) {
        unsigned j = (h + i) % CYC_SLOTS;
        if (cyc_tab[j].cnt == 0 || cyc_tab[j].key == key) {
            cyc_tab[j].key = key;
            return j;
        }
    }
    return 0;
}

void swiv_cycles_flush(void)
{
    if (!cyc_tab) return;
    const char *path = getenv("SWIV_CYCLES");
    FILE *f = path ? fopen(path, "w") : NULL;
    if (f) {
        for (unsigned j = 0; j < CYC_SLOTS; j++)
            if (cyc_tab[j].cnt)
                fprintf(f, "%06x %06x %u\n",
                        (unsigned)(cyc_tab[j].key >> 24) & 0xffffff,
                        (unsigned)(cyc_tab[j].key & 0xffffff),
                        (cyc_tab[j].sum + cyc_tab[j].cnt / 2) /
                        cyc_tab[j].cnt);
        fclose(f);
    }
}

static void cycles_hook(unsigned int pc)
{
    if (cyclog_state < 0) {
        cyclog_state = getenv("SWIV_CYCLES") ? 1 : 0;
        if (cyclog_state) cyc_tab = calloc(CYC_SLOTS, sizeof *cyc_tab);
    }
    if (!cyclog_state) return;
    int now = cpu_cycles_run();
    int delta = now - cyc_last;
    if (delta > 0 && delta < 256 && cyc_last_pc) {
        uint64_t key = ((uint64_t)cyc_last_pc << 24) | (pc & 0xffffff);
        unsigned j = cyc_slot(key);
        cyc_tab[j].sum += (unsigned)delta;
        cyc_tab[j].cnt++;
    }
    cyc_last = now;
    cyc_last_pc = pc;
}

/* SWIV_RTNCYC=lo,lo,...:  measure what a whole subroutine costs.
 *
 * A native override replaces a routine's instructions with C, so it must
 * charge what those instructions charged or the CPU runs ahead of the
 * chipset.  The cost is data-dependent (these routines contain loops), so
 * it is measured per call and averaged rather than derived: enter at the
 * given pc, remember the stack depth, and close the measurement when the
 * pc leaves and the stack is back.
 */
/* cpu_cycles_run() counts within the current scanline timeslice and
 * restarts each line, so a routine that straddles a line boundary gets a
 * negative delta.  Fold the restarts into a monotonic count. */
static long mono_base; static int mono_last;
static long mono_cycles(void)
{
    int now = cpu_cycles_run();
    if (now < mono_last) mono_base += mono_last;
    mono_last = now;
    return mono_base + now;
}

static unsigned rtn_pcs[32]; static int rtn_count = -1;
static struct { unsigned long sum; unsigned cnt; uint32_t sp; int active;
                long entry_cyc; } rtn[32];

static void rtn_hook(unsigned int pc)
{
    if (rtn_count < 0) {
        rtn_count = 0;
        const char *spec = getenv("SWIV_RTNCYC");
        if (spec)
            for (const char *p2 = spec; *p2 && rtn_count < 32; ) {
                rtn_pcs[rtn_count++] = (unsigned)strtoul(p2, NULL, 16);
                const char *c = strchr(p2, ','); if (!c) break; p2 = c + 1;
            }
    }
    if (!rtn_count) return;
    uint32_t sp = cpu_get_reg(CPU_REG_A7);
    long now = mono_cycles();
    for (int i = 0; i < rtn_count; i++) {
        if (rtn[i].active && sp > rtn[i].sp) {      /* the rts popped */
            long d = now - rtn[i].entry_cyc;
            if (d > 0 && d < 1000000) { rtn[i].sum += (unsigned long)d; rtn[i].cnt++; }
            rtn[i].active = 0;
        }
        if (!rtn[i].active && pc == rtn_pcs[i]) {
            rtn[i].active = 1; rtn[i].sp = sp; rtn[i].entry_cyc = now;
        }
    }
}

void swiv_rtncyc_flush(void)
{
    if (rtn_count <= 0) return;
    for (int i = 0; i < rtn_count; i++)
        if (rtn[i].cnt)
            fprintf(stderr, "rtncyc $%06x %lu cycles mean over %u calls\n",
                    rtn_pcs[i], rtn[i].sum / rtn[i].cnt, rtn[i].cnt);
}

void swiv_instr_hook(unsigned int pc)
{
    cycles_hook(pc);
    rtn_hook(pc);
    if (recomp_trace_on) recomp_trace(pc);
    if (sfxlog_state) sfxlog_hook(pc);
    pc_history[pc_history_at] = pc;
    pc_history_at = (pc_history_at + 1) % PC_HISTORY;
    /* Every resload entry point is a distinct address in a table no 68000
     * code occupies, so servicing WHDLoad is a PC comparison and needs no
     * stub code in emulated memory. */
    if (whdload_trap(pc)) {
        /* The hook fires once per executed instruction.  When it changes
         * the pc -- which servicing a resload call always does -- the
         * instruction at the NEW pc runs without a second hook call, so
         * it never reaches the executed-pc set.  Every instruction after
         * a resload return would then be missing from a recompilation
         * built off that set.  Record it explicitly. */
        if (recomp_trace_on) recomp_trace_pc_only(cpu_get_reg(CPU_REG_PC));
        return;
    }
    if (pc_hook) pc_hook(pc);
}

static void update_display_top(void)
{
    display_top_valid = true;
    if (getenv("SWIV_DISPLAY_TOP")) {      /* diagnostic: pin the origin */
        display_top = atoi(getenv("SWIV_DISPLAY_TOP"));
        return;
    }

    /* Horizontal: a window that fits sits at its true screen position; one
     * wider than the buffer is centred on what it can show. */
    int window_start = diwstrt & 0xff;
    int window_width = (int)((diwstop & 0xff) | 0x100) - window_start;
    if (window_width > SCREEN_W)
        display_left = window_start + (window_width - SCREEN_W) / 2;
    else
        display_left = DISPLAY_ORIGIN_X;

    /* Vertical: the fixed raster origin, so every window lands where it
     * really is.  The room this leaves above a window is what shows sprites
     * drawn over the border, which the hardware does not clip vertically.
     * Only a window too tall to fit is pulled up. */
    int window_top = (diwstrt >> 8) & 0xff;
    int window_bottom = display_vstop();
    display_top = DISPLAY_ORIGIN_Y;
    if (window_bottom - display_top > SCREEN_H)
        display_top = window_bottom - SCREEN_H;
    if (display_top > window_top) display_top = window_top;
}

void amiga_run_frame(void)
{
    if (video_enabled) {
        update_display_top();
        uint32_t background = rgb4(color[0]);
        for (int pixel = 0; pixel < SCREEN_W * SCREEN_H; pixel++)
            framebuf[pixel] = background;
        copper_start();
    }
    frame_bpl0 = bplpt[0];
    swiv_sprite_draw_count = 0;
    for (cur_line = 0; cur_line < LINES_PER_FRAME && !stopped; cur_line++) {
        memcpy(color_line_start, color, sizeof color_line_start);
        color_change_count = 0;
        cop_h = 0;
        memcpy(render_bplpt, bplpt, sizeof(render_bplpt));
        if (video_enabled && (dmacon & 0x0280) == 0x0280)
            copper_run_line(cur_line);
        cpu_execute(CYCLES_PER_LINE);
        ciab_tick();
        if (video_enabled) {
            render_line(cur_line);
            if ((dmacon & 0x0220) == 0x0220)
                render_sprites_line(cur_line);
            paint_written_sprites(cur_line);
        }
        kbd_pump();
    }
    kbd_age();
    kbd_pump();
    display_top_valid = false;
    intreq |= 0x0020;
    irq_update();
    swiv_frame_no++;
}

void amiga_stop(void) { stopped = true; }

bool amiga_stopped(void) { return stopped; }

void amiga_report(void)
{
    char instruction[128];
    unsigned pc = cpu_get_reg(CPU_REG_PC);
    cpu_disassemble(instruction, pc);
    fprintf(stderr,
            "native: frames=%ld pc=$%06x files=%ld blits=%ld audio=%ld "
            "energy=%llu copper=%ld pixels=%ld dmacon=$%04x "
            "intena=$%04x "
            "intreq=$%04x next=%s\n",
            swiv_frame_no, pc, swiv_disk_load_count, swiv_blit_count,
            swiv_audio_writes, audio_energy, swiv_copper_moves,
            swiv_nonblack_pixels, dmacon,
            intena, intreq, instruction);
    fprintf(stderr,
            "  video: bplcon0=$%04x bplcon1=$%04x bplcon2=$%04x "
            "ddf=$%04x-$%04x diw=$%04x-$%04x\n",
            bplcon0, bplcon1, bplcon2, ddfstrt, ddfstop,
            diwstrt, diwstop);
    for (int channel = 0; channel < 4; channel++) {
        AudioChannel *state = &audio[channel];
        fprintf(stderr,
                "  aud%d: on=%d lc=$%06x play=$%06x pos=%u bytes=%u "
                "len=%u per=%u vol=%u\n",
                channel, state->on, state->lc, state->lc_play, state->pos,
                state->nbytes_play, state->lenlatch, state->period,
                state->volume);
    }
}

uint32_t amiga_get_coplc(void) { return cop1lc; }

void amiga_get_voice(int channel, AmigaVoice *out)
{
    if (!out) return;
    if (channel < 0 || channel > 3) { memset(out, 0, sizeof *out); return; }
    const AudioChannel *state = &audio[channel];
    out->lc = state->lc;
    out->lc_play = state->lc_play;
    out->pos = state->pos;
    out->nbytes_play = state->nbytes_play;
    out->lenlatch = state->lenlatch;
    out->period = state->period;
    out->volume = state->volume;
    out->on = state->on;
}

void amiga_init(void)
{
    memset(chip, 0, sizeof chip);
    memset(framebuf, 0, sizeof framebuf);
    memset(&ciab, 0, sizeof ciab);
    memset(&ciaa, 0, sizeof ciaa);
    memset(bplpt, 0, sizeof bplpt);
    memset(sprpt, 0, sizeof sprpt);
    memset(color, 0, sizeof color);
    memset(bltpt, 0, sizeof bltpt);
    memset(bltmod, 0, sizeof bltmod);
    memset(bltdat, 0, sizeof bltdat);
    memset(audio, 0, sizeof audio);
    memset(audio_ring, 0, sizeof audio_ring);
    memset(joy_state, 0, sizeof joy_state);
    kbd_head = kbd_tail = 0;
    kbd_sdr = 0xff;
    ciaa_icr_flags = ciaa_icr_mask = 0;
    kbd_pending = false;
    stopped = false;
    cur_line = 0;
    display_top_valid = false;
    swiv_frame_no = swiv_blit_count = swiv_disk_load_count = 0;
    swiv_copper_moves = swiv_nonblack_pixels = swiv_audio_writes = 0;
    audio_write_pos = audio_read_pos = 0;
    atomic_store_explicit(&audio_fill, 0, memory_order_release);
    audio_filter_left = audio_filter_right = 0;
    audio_energy = 0;
    audio_transition_muted = false;
    dmacon = 0x03c0;                  /* WHDLoad slave handoff state */
    intena = 0x4000;
    intreq = 0;
    cop1lc = cop2lc = cop_pc = 0;
    cop_wait_line = -1;
    bplcon0 = bplcon1 = bplcon2 = 0;
    bpl1mod = bpl2mod = 0;
    diwstrt = 0x2c81; diwstop = 0x2cc1;
    ddfstrt = 0x0038; ddfstop = 0x00d0;
    bltcon0 = bltcon1 = 0;
    blt_zero = true;
    bltafwm = bltalwm = 0xffff;
    swiv_playfield_shift = 0;
    if (getenv("SWIV_PLAYFIELD_SHIFT"))
        swiv_playfield_shift = atoi(getenv("SWIV_PLAYFIELD_SHIFT"));
    /* ws_Flags has WHDLF_ClearMem set, so both RAM regions start at zero. */
    memset(fast, 0, sizeof fast);
    cpu_init();
    cpu_reset();
}

void amiga_set_pc_hook(SwivPcHook hook) { pc_hook = hook; }

void amiga_get_palette(uint16_t out[32])
{
    for (int i = 0; i < 32; i++) out[i] = color[i] & 0x0fff;
}

uint32_t amiga_bplpt0(void) { return frame_bpl0; }

/* The framebuffer rows the display window actually occupies, so a frontend
 * can put something of its own in the border without ever covering the
 * picture. */
void amiga_palette(uint16_t *out)
{
    for (int i = 0; i < 32; i++) out[i] = color[i];
}

void amiga_display_bounds(int *first_row, int *last_row)
{
    int top = ((diwstrt >> 8) & 0xff) - display_top;
    int bottom = display_vstop() - display_top;
    if (top < 0) top = 0;
    if (bottom > SCREEN_H) bottom = SCREEN_H;
    if (first_row) *first_row = top;
    if (last_row) *last_row = bottom;
}

/* Display state, for diagnosing what a title programs frame by frame. */
void amiga_display_state(uint16_t *out_bplcon0, uint16_t *out_dmacon,
                         uint16_t *out_diwstrt, uint16_t *out_diwstop)
{
    if (out_bplcon0) *out_bplcon0 = bplcon0;
    if (out_dmacon) *out_dmacon = dmacon;
    if (out_diwstrt) *out_diwstrt = diwstrt;
    if (out_diwstop) *out_diwstop = diwstop;
}

/* Return from a hooked subroutine as if it had run and RTS'd. */
void amiga_return_from_hook(void)
{
    uint32_t stack = cpu_get_reg(CPU_REG_A7);
    cpu_set_reg(CPU_REG_A7, stack + 4);
    cpu_set_reg(CPU_REG_PC, rl(stack));
}

void amiga_enable_video(bool enabled)
{
    video_enabled = enabled;
}

int amiga_blitter_selftest(void)
{
    int failures = 0;
#define CHECK(condition, name) do {                                      \
    if (!(condition)) {                                                  \
        fprintf(stderr, "blitter self-test: FAIL %s\n", name);          \
        failures++;                                                      \
    }                                                                    \
} while (0)
#define RESET_BLITTER() do {                                             \
    memset(chip + 0x1000, 0, 0x1000);                                   \
    bltcon0 = bltcon1 = 0;                                               \
    bltafwm = bltalwm = 0xffff;                                          \
    memset(bltpt, 0, sizeof bltpt);                                      \
    memset(bltmod, 0, sizeof bltmod);                                    \
    memset(bltdat, 0, sizeof bltdat);                                    \
    blt_zero = true;                                                     \
} while (0)

    /* Straight A -> D copy. */
    RESET_BLITTER();
    ww(0x1000, 0xabcd);
    bltcon0 = 0x09f0;
    bltpt[0] = 0x1000;
    bltpt[3] = 0x1100;
    blit((1 << 6) | 1);
    CHECK(rw(0x1100) == 0xabcd, "A-to-D copy");

    /* Cookie-cut: D = (A & B) | (~A & C). */
    RESET_BLITTER();
    ww(0x1000, 0xff00);
    ww(0x1100, 0x1234);
    ww(0x1200, 0x5678);
    bltcon0 = 0x0fca;
    bltpt[0] = 0x1000;
    bltpt[1] = 0x1100;
    bltpt[2] = 0x1200;
    bltpt[3] = 0x1300;
    blit((1 << 6) | 1);
    CHECK(rw(0x1300) == 0x1278, "cookie-cut minterm");

    /* BZERO readback semantics: A AND NOT C, first nonzero then zero. */
    RESET_BLITTER();
    ww(0x1000, 0xf000);
    ww(0x1200, 0x0fff);
    bltcon0 = 0x0a50;
    bltpt[0] = 0x1000;
    bltpt[2] = 0x1200;
    blit((1 << 6) | 1);
    CHECK(!blt_zero, "BZERO clear");
    ww(0x1000, 0x0f00);
    ww(0x1200, 0xffff);
    bltpt[0] = 0x1000;
    bltpt[2] = 0x1200;
    blit((1 << 6) | 1);
    CHECK(blt_zero, "BZERO set");

    /* First/last-word masks are applied before the A shifter. */
    RESET_BLITTER();
    ww(0x1000, 0xaaaa);
    ww(0x1002, 0xbbbb);
    bltcon0 = 0x09f0;
    bltafwm = 0x00ff;
    bltalwm = 0xff00;
    bltpt[0] = 0x1000;
    bltpt[3] = 0x1100;
    blit((1 << 6) | 2);
    CHECK(rw(0x1100) == 0x00aa && rw(0x1102) == 0xbb00,
          "first/last masks");

    /* A/D modulos advance between rows. */
    RESET_BLITTER();
    ww(0x1000, 0x1111);
    ww(0x1002, 0x2222);
    ww(0x1004, 0x3333);
    bltcon0 = 0x09f0;
    bltpt[0] = 0x1000;
    bltpt[3] = 0x1100;
    bltmod[0] = 2;
    bltmod[3] = 2;
    blit((2 << 6) | 1);
    CHECK(rw(0x1100) == 0x1111 && rw(0x1104) == 0x3333,
          "row modulos");

    /* Ascending A shift carries the previous source word. */
    RESET_BLITTER();
    ww(0x1000, 0x1234);
    ww(0x1002, 0x5678);
    bltcon0 = 0x49f0;
    bltpt[0] = 0x1000;
    bltpt[3] = 0x1100;
    blit((1 << 6) | 2);
    CHECK(rw(0x1100) == 0x0123 && rw(0x1102) == 0x4567,
          "ascending A shift");

    /* Descending traversal copies words from high to low addresses. */
    RESET_BLITTER();
    ww(0x1000, 0x1357);
    ww(0x1002, 0x2468);
    bltcon0 = 0x09f0;
    bltcon1 = 0x0002;
    bltpt[0] = 0x1002;
    bltpt[3] = 0x1102;
    blit((1 << 6) | 2);
    CHECK(rw(0x1100) == 0x1357 && rw(0x1102) == 0x2468,
          "descending copy");

#undef RESET_BLITTER
#undef CHECK
    if (failures) {
        fprintf(stderr, "blitter self-test: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "blitter self-test: PASS (8 cases)\n");
    return 0;
}

int amiga_video_selftest(void)
{
    int failures = 0;
#define VCHECK(condition, name) do {                                    \
    if (!(condition)) {                                                 \
        fprintf(stderr, "video self-test: FAIL %s\n", name);           \
        failures++;                                                     \
    }                                                                   \
} while (0)
    memset(chip, 0, sizeof chip);
    memset(framebuf, 0, sizeof framebuf);
    memset(bplpt, 0, sizeof bplpt);
    memset(sprpt, 0, sizeof sprpt);
    memset(color, 0, sizeof color);
    dmacon = 0x03a0;
    diwstrt = 0x2c81; diwstop = 0x2cc1;
    ddfstrt = 0x0038; ddfstop = 0x00d0;
    bplcon0 = 0x1000;
    bplcon2 = 0;
    bpl1mod = bpl2mod = 0;
    bplpt[0] = 0x2000;
    color[1] = 0xf00;
    chip[0x2000] = 0x80;
    memcpy(render_bplpt, bplpt, sizeof(render_bplpt));
    render_line(0x2c);
    VCHECK(framebuf[0] == rgb4(0xf00), "bitplane set pixel");
    VCHECK(framebuf[1] == rgb4(0x000), "bitplane clear pixel");
    VCHECK(bplpt[0] == 0x2000u + (uint32_t)fetch_bytes(),
           "bitplane DMA advance");

    memset(framebuf, 0, sizeof framebuf);
    bplpt[0] = 0x2000;
    bplcon1 = 3;
    memcpy(render_bplpt, bplpt, sizeof(render_bplpt));
    render_line(0x2c);
    VCHECK(framebuf[0] == rgb4(0x000), "fine-scroll leading pixel");
    VCHECK(framebuf[3] == rgb4(0xf00), "fine-scroll pixel delay");
    bplcon1 = 0;

    /* Battle Squadron's smooth-scroll setup fetches a word early.  With a
     * 15-pixel delay, source pixel one must already reach visible pixel zero;
     * otherwise the map blanks/jumps at every 16-pixel coarse rollover. */
    memset(framebuf, 0, sizeof framebuf);
    memset(chip + 0x2000, 0, 64);
    bplpt[0] = 0x2000;
    ddfstrt = 0x0030;
    ddfstop = 0x00d0;
    bplcon1 = 15;
    chip[0x2000] = 0x40;
    memcpy(render_bplpt, bplpt, sizeof(render_bplpt));
    fprintf(stderr, "debug: bplpt[0]=%06x render_bplpt[0]=%06x\n", bplpt[0], render_bplpt[0]);
    render_line(0x2c);
    VCHECK(framebuf[0] == rgb4(0xf00), "early-fetch fine-scroll lead");
    bplcon1 = 0;
    ddfstrt = 0x0038;

    ww(0x1000, 0x0180); ww(0x1002, 0x000f);
    ww(0x1004, 0xffff); ww(0x1006, 0xfffe);
    cop1lc = 0x1000;
    color[0] = 0;
    swiv_copper_moves = 0;
    copper_start();
    copper_run_line(0);
    VCHECK(color[0] == 0x00f, "copper MOVE");
    VCHECK(swiv_copper_moves == 1, "copper MOVE count");
    VCHECK(cop_wait_line == -1, "copper end marker");

    memset(framebuf, 0, sizeof framebuf);
    memset(sprpt, 0, sizeof sprpt);
    sprpt[0] = 0x3000;
    sprpt[1] = 0x3100;
    ww(0x3000, 0x2c40); ww(0x3002, 0x2e01);
    ww(0x3004, 0x8000); ww(0x3006, 0x8000); /* even value 3 */
    ww(0x3008, 0x8000); ww(0x300a, 0x8000);
    ww(0x3100, 0x2c40); ww(0x3102, 0x2e81); /* odd ATTACH */
    ww(0x3104, 0x0000); ww(0x3106, 0x8000); /* odd value 2 */
    ww(0x3108, 0x0000); ww(0x310a, 0x8000);
    color[27] = 0xabc; /* 3 | (2 << 2) = 11; attached bank starts at 16 */
    render_sprites_line(0x2c);
    color[27] = 0xdef;
    render_sprites_line(0x2d);
    VCHECK(framebuf[0] == rgb4(0xabc), "attached sprite palette");
    VCHECK(framebuf[1] == 0, "attached sprite transparent pixel");
    VCHECK(framebuf[SCREEN_W] == rgb4(0xdef),
           "scanline sprite palette change");

#undef VCHECK
    if (failures) {
        fprintf(stderr, "video self-test: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "video self-test: PASS (12 cases)\n");
    return 0;
}

int amiga_input_selftest(void)
{
    int failures = 0;
#define ICHECK(condition, name) do {                                    \
    if (!(condition)) {                                                 \
        fprintf(stderr, "input self-test: FAIL %s\n", name);           \
        failures++;                                                     \
    }                                                                   \
} while (0)
    memset(joy_state, 0, sizeof joy_state);
    joy_state[0] = 0x01;
    ICHECK(custom_read(0x00a) == 0x0100, "up quadrature");
    joy_state[0] = 0x02;
    ICHECK(custom_read(0x00a) == 0x0001, "down quadrature");
    joy_state[0] = 0x04;
    ICHECK(custom_read(0x00a) == 0x0300, "left quadrature");
    joy_state[0] = 0x08;
    ICHECK(custom_read(0x00a) == 0x0003, "right quadrature");
    joy_state[0] = 0x30;
    ICHECK((cia_read(0xbfe001) & 0x40) == 0, "primary fire active low");
    ICHECK((custom_read(0x016) & 0x0004) == 0,
           "secondary fire active low");

    kbd_head = kbd_tail = 0;
    kbd_pending = false;
    intreq = 0;
    amiga_key_event(0x44, false);
    kbd_pump();
    ICHECK(kbd_sdr == 0x77, "raw Return serial encoding");
    ICHECK((intreq & 0x0008) != 0, "keyboard PORTS interrupt");
    cia_write(0xbfec01, 0);
    ICHECK(!kbd_pending, "keyboard handshake");

#undef ICHECK
    if (failures) {
        fprintf(stderr, "input self-test: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "input self-test: PASS (9 cases)\n");
    return 0;
}

int amiga_audio_selftest(void)
{
    int failures = 0;
#define ACHECK(condition, name) do {                                    \
    if (!(condition)) {                                                 \
        fprintf(stderr, "audio self-test: FAIL %s\n", name);           \
        failures++;                                                     \
    }                                                                   \
} while (0)
    memset(audio, 0, sizeof audio);
    memset(audio_ring, 0, sizeof audio_ring);
    audio_write_pos = audio_read_pos = 0;
    atomic_store_explicit(&audio_fill, 0, memory_order_release);
    audio_filter_left = audio_filter_right = 0;
    dmacon = intreq = 0;
    chip[0x3000] = 100;
    chip[0x3001] = (uint8_t)-100;
    chip[0x3002] = 80;
    chip[0x3003] = (uint8_t)-80;
    custom_write(0x0a0, 0);
    custom_write(0x0a2, 0x3000);
    custom_write(0x0a4, 2);
    custom_write(0x0a6, 124);
    custom_write(0x0a8, 64);
    custom_write(0x096, 0x8201);
    ACHECK(audio[0].on, "DMA start");
    ACHECK(audio[0].nbytes_play == 4, "sample length latch");
    int16_t mixed[64 * 2];
    audio_mix(mixed, 64);
    long left_energy = 0, right_energy = 0;
    for (int frame = 0; frame < 64; frame++) {
        left_energy += labs(mixed[frame * 2]);
        right_energy += labs(mixed[frame * 2 + 1]);
    }
    ACHECK(left_energy > 0, "non-silent output");
    ACHECK(left_energy > right_energy, "Paula stereo panning");
    ACHECK((intreq & 0x0080) != 0, "sample-loop interrupt");
    amiga_audio_frame();
    ACHECK(amiga_audio_fill() == AUDIO_RATE / 50, "per-frame buffering");
    int16_t pulled[100 * 2];
    ACHECK(amiga_audio_pull(pulled, 100) == 100, "ring pull");
    custom_write(0x096, 0x0001);
    ACHECK(!audio[0].on, "DMA stop");

#undef ACHECK
    if (failures) {
        fprintf(stderr, "audio self-test: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "audio self-test: PASS (8 cases)\n");
    return 0;
}
