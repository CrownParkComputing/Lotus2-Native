/* recomp_verify -- run generated code across a snapshot pair.
 *
 * The pairs were captured for hand-porting, but they gate anything: load
 * the entry image and registers, run from the entry pc until the pc
 * reaches the exit snapshot's, then demand the memory and all sixteen
 * registers match the exit image.  Generated and hand-written ports are
 * judged by exactly the same standard.
 *
 *   build/recomp_verify re/pipeline/road/sp_0_2169dc re/pipeline/road/sp_1_2169de
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68krt.h"

#define CHIP 0x80000u
#define FAST 0x890000u

void lotus2_recomp_run(M68K *m, uint32_t stop_pc);

/* the gate's memory: flat snapshot images, custom registers inert */
static M68K *g_m;
uint32_t m68krt_read(uint32_t a, int sz)
{
    M68K *m = g_m;
    if ((a & 0xfff000u) == 0xdff000u) return m->custom[(a & 0x1fe) >> 1];
    uint8_t *p = a < m->chip_size ? m->chip + a
               : (a >= 0x200000u && a < 0x200000u + m->fast_size)
                 ? m->fast + (a - 0x200000u) : 0;
    if (!p) return 0;
    if (sz == 1) return p[0];
    if (sz == 2) return ((uint32_t)p[0] << 8) | p[1];
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
void m68krt_write(uint32_t a, int sz, uint32_t v)
{
    M68K *m = g_m;
    if ((a & 0xfff000u) == 0xdff000u) { m->custom[(a & 0x1fe) >> 1] = (uint16_t)v; return; }
    uint8_t *p = a < m->chip_size ? m->chip + a
               : (a >= 0x200000u && a < 0x200000u + m->fast_size)
                 ? m->fast + (a - 0x200000u) : 0;
    if (!p) return;
    if (sz == 1) { p[0] = (uint8_t)v; return; }
    if (sz == 2) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return; }
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint8_t *load(const char *path, size_t want)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s\n", path); return NULL; }
    uint8_t *p = calloc(1, want);
    size_t n = fread(p, 1, want, f);
    fclose(f);
    (void)n;
    return p;
}

typedef struct { uint32_t d[8], a[8], pc; } Regs;

static int load_regs(const char *prefix, Regs *r)
{
    char path[512];
    snprintf(path, sizeof path, "%s.regs", prefix);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "open %s\n", path); return 0; }
    char k[32]; unsigned v;
    memset(r, 0, sizeof *r);
    while (fscanf(f, "%31s %x", k, &v) == 2) {
        if (!strcmp(k, "pc")) r->pc = v;
        else if (k[0] == 'd' && k[1] >= '0' && k[1] <= '7' && !k[2]) r->d[k[1]-'0'] = v;
        else if (k[0] == 'a' && k[1] >= '0' && k[1] <= '7' && !k[2]) r->a[k[1]-'0'] = v;
    }
    fclose(f);
    return 1;
}

static uint8_t *load_side(const char *prefix, const char *side, size_t want)
{
    char path[512];
    snprintf(path, sizeof path, "%s_%s.bin", prefix, side);
    return load(path, want);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: recomp_verify ENTRY EXIT\n"); return 2; }
    const char *ep = argv[1], *xp = argv[2];
    const char *name = argc > 3 ? argv[3] : ep;

    Regs er, xr;
    if (!load_regs(ep, &er) || !load_regs(xp, &xr)) return 1;

    M68K m;
    memset(&m, 0, sizeof m);
    m.chip = load_side(ep, "chip", CHIP);
    m.fast = load_side(ep, "fast", FAST);
    m.chip_size = CHIP; m.fast_size = FAST;
    g_m = &m;
    if (!m.chip || !m.fast) return 1;
    for (int i = 0; i < 8; i++) { m.d[i] = er.d[i]; m.a[i] = er.a[i]; }
    m.pc = er.pc;

    uint8_t *wchip = load_side(xp, "chip", CHIP);
    uint8_t *wfast = load_side(xp, "fast", FAST);
    if (!wchip || !wfast) return 1;

    long steps = 0;
    for (; steps < 40000000L && !m.halted && m.pc != xr.pc; steps++) {
        uint32_t before = m.pc;
        lotus2_recomp_run(&m, xr.pc);
        if (m.pc == before && !m.halted) break;
        if (m.pc == xr.pc || m.halted) break;
    }
    if (m.halted) {
        printf("%-42s HALTED: %s (pc $%06x)\n", name,
               m.fault ? m.fault : "?", m.pc);
        return 1;
    }
    if (m.pc != xr.pc) {
        printf("%-42s never reached exit pc $%06x (stopped $%06x)\n",
               name, xr.pc, m.pc);
        return 1;
    }

    long bad = 0; int shown = 0;
    for (uint32_t i = 0; i < FAST; i++) {
        uint32_t addr = 0x200000u + i;
        if (addr >= 0x217c00 && addr < 0x217e00) continue;   /* stack page */
        if (m.fast[i] != wfast[i]) {
            if (shown++ < 6)
                printf("  %s $%06x: recomp $%02x oracle $%02x\n",
                       name, addr, m.fast[i], wfast[i]);
            bad++;
        }
    }
    for (uint32_t i = 0; i < CHIP; i++)
        if (m.chip[i] != wchip[i]) {
            if (shown++ < 6)
                printf("  %s chip $%06x: recomp $%02x oracle $%02x\n",
                       name, i, m.chip[i], wchip[i]);
            bad++;
        }
    for (int i = 0; i < 8; i++) {
        if (m.d[i] != xr.d[i]) {
            printf("  %s d%d: recomp $%08x oracle $%08x\n", name, i, m.d[i], xr.d[i]);
            bad++;
        }
        if (i != 7 && m.a[i] != xr.a[i]) {
            printf("  %s a%d: recomp $%08x oracle $%08x\n", name, i, m.a[i], xr.a[i]);
            bad++;
        }
    }
    printf("%-42s %s (%ld diffs)\n", name, bad ? "FAIL" : "EXACT", bad);
    return bad != 0;
}
