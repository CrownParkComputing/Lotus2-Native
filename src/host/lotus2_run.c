/* Headless Lotus Turbo Challenge 2 (CD32, 1991) runner: boot the WHDLoad
 * install and report what happened.
 *
 * Lotus 2 is a CD32 title (AGA chipset) but otherwise follows the same
 * WHDLoad conventions as SWIV.  The slave is 6296 bytes and requests
 * 0.5 MB ChipMem (per its own ws_BaseMemSize) plus ~8.5 MB ExpMem
 * (suspicious but the file says so, so honor it).
 *
 * The host is the same OCS/AGA Musashi implementation used for SWIV; the
 * only title-specific bits are the CLI defaults and the install layout.
 * Specifically: 0.5 MB chip (the SWIV default), the slave in
 * original/Lotus2CD32/Lotus2CD32.slave, Disk.1 as a RawDIC dump at
 * original/Lotus2CD32/Disk.1 (the disk_loader() reads it as a flat byte
 * image the way WHDLoad's imager does).
 */
#include "amiga.h"
#include "whdload.h"
#include "m68k.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DUMPS 8
static long dump_file_base[MAX_DUMPS], dump_file_count[MAX_DUMPS];
static const char *dump_file_path[MAX_DUMPS];
static int dump_files = 0;
/* --dump-seq PREFIX BASE COUNT: with --ppm-seq, also write PREFIX%05ld.bin
 * (guest RAM BASE..BASE+COUNT) every --ppm-every frames, so a screenshot and
 * the game state it was drawn from come out of the same run. */
static long dump_seq_base = -1, dump_seq_count = 0;
static const char *dump_seq_prefix = NULL;

static void dump_range(const char *path, long base, long count)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    for (long o = 0; o < count; o += 2) {
        unsigned w = m68k_read_memory_16((unsigned)(base + o));
        fputc(w >> 8, f);
        fputc(w & 0xff, f);
    }
    fclose(f);
}

/* --ptrlog PATH SPEC: a title-neutral object log (ported verbatim from
 * SWIV's swiv_run.c).  SPEC is
 * "ADDR[:N][~off]=off.size,off.size,...[@off.size,...]" -- per frame walk
 * the objects and log one line each: frame, list index, pointer, then each
 * field (size b/w/l; b prints hex, w/l signed decimal).
 *   ADDR:N        an array of N longword object pointers (stops at 0)
 *   ADDR~off      a linked list headed at ADDR, next pointer at +off
 *   ADDR:N~off    N list heads at ADDR, each a linked list (next at +off)
 * The trailing "@..." section logs globals once per frame as a "G" line. */
static FILE *ptrlog; static const char *ptrlog_spec;
static void ptrlog_fields(unsigned obj, const char *fields)
{
    const char *f = fields;
    while (f && *f) {
        unsigned off = 0; char size = 'w';
        if (sscanf(f, "%x.%c", &off, &size) >= 1) {
            if (size == 'b') fprintf(ptrlog, " %02x", m68k_read_memory_8(obj + off));
            else if (size == 'l') fprintf(ptrlog, " %d", (int)m68k_read_memory_32(obj + off));
            else fprintf(ptrlog, " %d", (short)m68k_read_memory_16(obj + off));
        }
        f = strchr(f, ',');
        if (f) f++;
    }
}
static void ptrlog_frame(long frame)
{
    unsigned base = 0, max = 1, nextoff = 0; int linked = 0;
    const char *eq = strchr(ptrlog_spec, '=');
    const char *tl = strchr(ptrlog_spec, '~');
    char fields[256] = "";
    if (eq) {
        snprintf(fields, sizeof fields, "%s", eq + 1);
        char *cut = strchr(fields, '@');
        if (cut) *cut = 0;
    }
    if (tl && (!eq || tl < eq)) { linked = 1; sscanf(tl + 1, "%x", &nextoff); }
    sscanf(ptrlog_spec, "%x:%u", &base, &max);
    const char *at = strchr(ptrlog_spec, '@');
    if (at) {
        fprintf(ptrlog, "%ld G", frame);
        ptrlog_fields(0, at + 1);
        fputc('\n', ptrlog);
    }
    for (unsigned i = 0; i < max; i++) {
        unsigned obj = m68k_read_memory_32(base + 4 * i);
        if (!obj && !linked) break;
        int guard = 0;
        while (obj && guard++ < 1024) {
            fprintf(ptrlog, "%ld %u %06x", frame, i, obj);
            ptrlog_fields(obj, fields);
            fputc('\n', ptrlog);
            if (!linked) break;
            obj = m68k_read_memory_32(obj + nextoff);
        }
    }
}

static long number(const char *text)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 0);
    if (errno || !end || *end) {
        fprintf(stderr, "not a number: %s\n", text);
        exit(2);
    }
    return value;
}

static int write_ppm(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        perror(path);
        return 1;
    }
    fprintf(file, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int pixel = 0; pixel < SCREEN_W * SCREEN_H; pixel++) {
        uint32_t value = framebuf[pixel];
        fputc((value >> 16) & 0xff, file);
        fputc((value >> 8) & 0xff, file);
        fputc(value & 0xff, file);
    }
    if (fclose(file)) {
        perror(path);
        return 1;
    }
    fprintf(stderr, "lotus2: wrote %s\n", path);
    return 0;
}

int main(int argc, char **argv)
{
    WhdConfig whd = {
        .dir = "original/Lotus2CD32",
        .slave = "Lotus2CD32.slave",
    };
    long frames = 600;
    long video_from = 0;
    long fire_from = -1, fire_period = 0;
    long expect_disk_loads = 0, expect_blits = 0;
    const char *ppm = NULL, *ppm_seq = NULL;
    long ppm_every = 50;
    bool trace = false, selftest = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) whd.dir = argv[++i];
        else if (!strcmp(argv[i], "--slave") && i + 1 < argc)
            whd.slave = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = number(argv[++i]);
        else if (!strcmp(argv[i], "--video-from") && i + 1 < argc)
            video_from = number(argv[++i]);
        else if (!strcmp(argv[i], "--no-video")) video_from = -1;
        else if (!strcmp(argv[i], "--fire-from") && i + 1 < argc)
            fire_from = number(argv[++i]);
        else if (!strcmp(argv[i], "--fire-period") && i + 1 < argc)
            fire_period = number(argv[++i]);
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--ppm-seq") && i + 1 < argc)
            ppm_seq = argv[++i];
        else if (!strcmp(argv[i], "--ppm-every") && i + 1 < argc)
            ppm_every = number(argv[++i]);
        else if (!strcmp(argv[i], "--custom") && i + 2 < argc) {
            long which = number(argv[++i]);
            long value = number(argv[++i]);
            if (which < 1 || which > 5) {
                fprintf(stderr, "--custom takes 1..5\n");
                return 2;
            }
            whd.custom[which - 1] = (uint32_t)value;
        }
        else if (!strcmp(argv[i], "--expect-disk-loads") && i + 1 < argc)
            expect_disk_loads = number(argv[++i]);
        else if (!strcmp(argv[i], "--expect-blits") && i + 1 < argc)
            expect_blits = number(argv[++i]);
        else if (!strcmp(argv[i], "--ptrlog") && i + 2 < argc) {
            ptrlog = fopen(argv[++i], "w");
            ptrlog_spec = argv[++i];
        }
        else if (!strcmp(argv[i], "--trace")) trace = true;
        else if (!strcmp(argv[i], "--selftest")) selftest = true;
        else if (!strcmp(argv[i], "--dump-seq") && i + 3 < argc) {
            dump_seq_prefix = argv[++i];
            dump_seq_base = number(argv[++i]);
            dump_seq_count = number(argv[++i]);
        }
        else if (!strcmp(argv[i], "--dump-file") && i + 3 < argc) {
            /* Binary snapshot of guest RAM after the run: the game's own
             * trackloader and decruncher have executed, so this is the clean
             * in-memory image for disassembly. */
            if (dump_files < MAX_DUMPS) {
                dump_file_base[dump_files] = number(argv[++i]);
                dump_file_count[dump_files] = number(argv[++i]);
                dump_file_path[dump_files++] = argv[++i];
            } else i += 3;
        }
        else {
            fprintf(stderr,
                    "usage: %s [--dir DIR] [--slave FILE] [--frames N]\n"
                    "       [--video-from N|--no-video] [--fire-from N]\n"
                    "       [--fire-period N] [--custom 1..5 VALUE]\n"
                    "       [--ppm FILE] [--ppm-seq PREFIX] [--ppm-every N]\n"
                    "       [--expect-disk-loads N] [--expect-blits N]\n"
                    "       [--dump-file BASE COUNT PATH] [--trace] [--selftest]\n",
                    argv[0]);
            return 2;
        }
    }

    if (selftest) {
        int failed = amiga_blitter_selftest();
        failed |= amiga_video_selftest();
        failed |= amiga_input_selftest();
        failed |= amiga_audio_selftest();
        return failed;
    }

    amiga_init();
    if (!whdload_boot(&whd)) return 1;
    amiga_enable_video(video_from == 0);

    long last_loads = -1;
    while (swiv_frame_no < frames && !amiga_stopped()) {
        if (video_from > 0 && swiv_frame_no == video_from)
            amiga_enable_video(true);
        if (fire_from >= 0 && swiv_frame_no >= fire_from) {
            joy_state[1] = fire_period
                ? (((swiv_frame_no / fire_period) % 2) ? 0x10 : 0x00)
                : 0x10;
            joy_state[0] = joy_state[1];
        }
        amiga_run_frame();
        amiga_audio_frame();
        if (ptrlog) ptrlog_frame(swiv_frame_no);
        if (ppm_seq && swiv_frame_no % ppm_every == 0) {
            char path[700];
            snprintf(path, sizeof path, "%s%05ld.ppm", ppm_seq, swiv_frame_no);
            write_ppm(path);
            if (dump_seq_prefix) {
                snprintf(path, sizeof path, "%s%05ld.bin", dump_seq_prefix,
                         swiv_frame_no);
                dump_range(path, dump_seq_base, dump_seq_count);
            }
        }
        if (trace || whd_disk_loads != last_loads ||
            swiv_frame_no % 100 == 0) {
            uint16_t bplcon0, dmacon, diwstrt, diwstop;
            amiga_display_state(&bplcon0, &dmacon, &diwstrt, &diwstop);
            fprintf(stderr, "frame %5ld pc=$%06x loads=%ld blits=%ld "
                    "bplcon0=$%04x dmacon=$%04x diw=$%04x-$%04x pixels=%ld\n",
                    swiv_frame_no, m68k_get_reg(NULL, M68K_REG_PC),
                    whd_disk_loads, swiv_blit_count, bplcon0, dmacon,
                    diwstrt, diwstop, swiv_nonblack_pixels);
            last_loads = whd_disk_loads;
        }
    }

    swiv_recomp_trace_flush();
    if (ptrlog) fclose(ptrlog);
    amiga_report();
    whdload_report();
    int status = 0;
    if (ppm) status |= write_ppm(ppm);
    for (int k = 0; k < dump_files; k++) {
        FILE *f = fopen(dump_file_path[k], "wb");
        if (!f) { perror(dump_file_path[k]); status = 1; }
        else {
            for (long o = 0; o < dump_file_count[k]; o += 2) {
                unsigned w = m68k_read_memory_16((unsigned)(dump_file_base[k] + o));
                fputc(w >> 8, f);
                fputc(w & 0xff, f);
            }
            fclose(f);
            fprintf(stderr, "lotus2: dumped $%06lx+$%lx to %s\n",
                    dump_file_base[k], dump_file_count[k], dump_file_path[k]);
        }
    }
    if (amiga_stopped()) {
        fprintf(stderr, "lotus2: STOPPED after %ld frames\n", swiv_frame_no);
        status = 1;
    }
    if (expect_disk_loads && whd_disk_loads != expect_disk_loads) {
        fprintf(stderr, "lotus2: expected %ld disk loads, got %ld\n",
                expect_disk_loads, whd_disk_loads);
        status = 1;
    }
    if (expect_blits && swiv_blit_count < expect_blits) {
        fprintf(stderr, "lotus2: expected at least %ld blits, got %ld\n",
                expect_blits, swiv_blit_count);
        status = 1;
    }
    return status;
}
