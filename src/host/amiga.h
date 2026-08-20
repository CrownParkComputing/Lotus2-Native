#ifndef SWIV_AMIGA_H
#define SWIV_AMIGA_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* ws_BaseMemSize: SWIV's slave asks for the whole 512K of chip RAM. */
#ifndef CHIP_SIZE
#define CHIP_SIZE 0x80000
#endif
/* The expansion RAM WHDLoad allocates for ws_ExpMem ($8B000 for SWIV), placed
 * where a real machine's Zorro II fast RAM starts.  The window is larger than
 * the request so the slave and the resload table can live above it, out of the
 * game's reach. */
#define FAST_BASE 0x200000u
#define FAST_SIZE 0x200000u
/* A whole PAL raster wide, not just the textbook 320: SWIV's playfield is 352
 * pixels of which the game shows a 320-pixel window, and a title that opens a
 * wider window would lose pixels off both sides at 320. */
#define SCREEN_W 352
/* A whole PAL raster, not a textbook 256-line screen.  The buffer has to be
 * tall enough that every window a title opens can be CENTRED in it with room
 * above for sprites, which are not clipped to the display window. */
#define SCREEN_H 288
#define LINES_PER_FRAME 312
#define CYCLES_PER_LINE 455

/* Musashi's callback is function-like, which modern CMake deliberately does
 * not pass as a command-line definition.  This header is force-included for
 * every core translation unit, so keep the shared hook spelling here. */
void swiv_recomp_trace_flush(void);
#ifndef M68K_INSTRUCTION_CALLBACK
#define M68K_INSTRUCTION_CALLBACK(pc) swiv_instr_hook(pc)
#endif

extern uint8_t chip[CHIP_SIZE];
extern uint8_t fast[FAST_SIZE];
extern uint32_t framebuf[SCREEN_W * SCREEN_H];
extern long swiv_frame_no;
extern long swiv_blit_count;
extern long swiv_disk_load_count;
extern long swiv_copper_moves;
extern long swiv_nonblack_pixels;
extern long swiv_audio_writes;
extern uint8_t joy_state[2];
/* Lores-pixel nudge of the playfield relative to the sprites, for checking a
 * title's DDFSTRT/DIWSTRT pairing against the real machine.  0 is the
 * host's own derivation. */
extern int swiv_playfield_shift;

/* A title-specific instruction hook: the host calls it with every PC, and
 * the title's own module decides what to intercept.  Per-title addresses
 * belong there, not in the chipset. */
typedef void (*SwivPcHook)(unsigned int pc);
void amiga_set_pc_hook(SwivPcHook hook);
/* Live chipset state for debug tools: the 32 colour registers as RGB4
 * words, and the current bitplane 0 pointer (which buffer is on screen). */
void amiga_get_palette(uint16_t out[32]);
uint32_t amiga_bplpt0(void);
void amiga_display_state(uint16_t *bplcon0, uint16_t *dmacon,
                         uint16_t *diwstrt, uint16_t *diwstop);
void amiga_display_bounds(int *first_row, int *last_row);
void amiga_palette(uint16_t *out);   /* 32 entries */

/* Replace-on-blit.  A title can claim a range of blit source addresses; when
 * the blitter draws from that range the original pixels are suppressed and a
 * request is recorded here instead, with the screen rectangle it would have
 * covered.  A frontend then draws whatever it likes there -- at any colour
 * depth or resolution, because nothing about that is the chipset's business
 * any more.  This is why the 32-colour limit is not a limit: it belongs to
 * the game's data, not to the renderer. */
typedef struct {
    int      x, y;          /* framebuffer position of the top-left */
    int      width, height; /* the size the original would have drawn */
    int      id;            /* whatever the title registered */
} SwivSpriteDraw;

extern SwivSpriteDraw swiv_sprite_draws[64];
extern int swiv_sprite_draw_count;

void amiga_register_replacement(uint32_t source_low, uint32_t source_high,
                                int id);
void amiga_clear_replacements(void);
void amiga_replacements_suppress(bool on);
void amiga_return_from_hook(void);

/* Reset the chipset and the CPU.  Nothing is loaded: the WHDLoad host places
 * the slave and supplies the entry vector afterwards. */
void amiga_init(void);
/* CPU-visible RAM, or NULL when the range is not RAM at all.  The loader
 * writes through this so a load that would run off the end of a region is
 * refused instead of wrapping into unrelated memory. */
uint8_t *amiga_ram(uint32_t address, uint32_t length);
void amiga_stop(void);
void amiga_run_frame(void);
void amiga_enable_video(bool enabled);
void amiga_key_event(uint8_t rawcode, bool up);
void amiga_audio_frame(void);
int amiga_audio_pull(int16_t *output, int frames);
int amiga_audio_fill(void);
void swiv_instr_hook(unsigned int pc);
void amiga_pc_history(void);
int amiga_blitter_selftest(void);
int amiga_video_selftest(void);
int amiga_input_selftest(void);
int amiga_audio_selftest(void);
bool amiga_stopped(void);
void amiga_report(void);
void amiga_dump_copper(FILE *fp);

unsigned int m68k_read_memory_8(unsigned int address);
unsigned int m68k_read_memory_16(unsigned int address);
unsigned int m68k_read_memory_32(unsigned int address);
void m68k_write_memory_8(unsigned int address, unsigned int value);
void m68k_write_memory_16(unsigned int address, unsigned int value);
void m68k_write_memory_32(unsigned int address, unsigned int value);

#endif
