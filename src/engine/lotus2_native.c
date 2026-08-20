/* lotus2_native.c -- native Lotus 2 engine runner (TRANSLATE stage).
 *
 * Two jobs today:
 *
 *   --render OUT.ppm      composite a frame natively from oracle RAM dumps
 *                         (mini-copper + planar decode), for the pixel-exact
 *                         gate against the oracle screenshot.
 *   --verify-verbs        run the ported §2 verbs on the dumped inputs and
 *                         diff their outputs word-for-word against the same
 *                         dump (the outputs are already in guest RAM).
 *
 * Inputs default to the frame-2000 gate pair:
 *   --chip re/pipeline/gate_chip_02000.bin   (chip RAM, $0..$80000)
 *   --base re/pipeline/gate_base_02000.bin   (base page, $208000+$4000)
 *   --coplist ADDR                           (default: try both known lists)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "compositor.h"

#define A3_BASE 0x208000u

static uint32_t framebuf[LOTUS2_SCREEN_W * LOTUS2_SCREEN_H];

static int diff_words(const char *what, const uint8_t *got, uint32_t got_off,
                      const uint8_t *want, uint32_t want_off, int words,
                      int stride_got, int stride_want)
{
    int bad = 0;
    for (int i = 0; i < words; i++) {
        uint16_t g = g16(got, got_off + (uint32_t)(i * stride_got));
        uint16_t w = g16(want, want_off + (uint32_t)(i * stride_want));
        if (g != w) {
            if (bad < 4)
                fprintf(stderr, "  %s[%d]: native $%04x oracle $%04x\n",
                        what, i, g, w);
            bad++;
        }
    }
    fprintf(stderr, "%s: %d/%d words match%s\n", what, words - bad, words,
            bad ? " FAIL" : "");
    return bad != 0;
}

static int verify_verbs(Game *g)
{
    int status = 0;

    /* build_fade: the base page's $320c may already hold the NEXT screen's
     * palette (it does at frame 2000), so verify self-consistently: bank 8
     * (step $10) is the identity, so feeding the dumped bank 8 back through
     * the formula must reproduce all 9 dumped banks word-for-word. */
    Game scratch = *g;
    scratch.chip = malloc(GUEST_CHIP_SIZE);
    scratch.base = malloc(GUEST_BASE_SIZE);
    memcpy(scratch.chip, g->chip, GUEST_CHIP_SIZE);
    memcpy(scratch.base, g->base, GUEST_BASE_SIZE);
    memcpy(scratch.base + G_PALETTE,
           g->chip + CHIP_FADE_BANKS + 8 * 0x40, 32 * 2);
    memset(scratch.chip + CHIP_FADE_BANKS, 0xa5, 9 * 0x40);
    build_fade(&scratch);
    status |= diff_words("build_fade $5400", scratch.chip, CHIP_FADE_BANKS,
                         g->chip, CHIP_FADE_BANKS, 9 * 32, 2, 2);

    /* load_palette: recompute the copper COLOR operands from the dumped
     * fade banks + bank selector, diff against the dumped operands */
    memcpy(scratch.chip + CHIP_FADE_BANKS,
           g->chip + CHIP_FADE_BANKS, 9 * 0x40);
    memset(scratch.chip + CHIP_COP_PALETTE, 0xa5, 32 * 4);
    load_palette(&scratch);
    status |= diff_words("load_palette $7ff74", scratch.chip,
                         CHIP_COP_PALETTE + 2, g->chip, CHIP_COP_PALETTE + 2,
                         32, 4, 4);

    /* build_copper_planes: plane count from the dumped BPLCON0 operand,
     * buffer = front screen from the base page; diff operands */
    int planes = (g16(g->chip, CHIP_COP_BPLCON0) >> 12) & 7;
    uint32_t front = g32(g->base, G_SCREEN_A);
    memset(scratch.chip + CHIP_COP_BPLPTRS, 0xa5, (size_t)planes * 8);
    p16(scratch.chip, CHIP_COP_BPLCON0, 0xa5a5);
    build_copper_planes(&scratch, planes, front);
    int ptr_fail = 0;
    ptr_fail |= diff_words("build_copper_planes bplcon0", scratch.chip,
                           CHIP_COP_BPLCON0, g->chip, CHIP_COP_BPLCON0,
                           1, 2, 2);
    ptr_fail |= diff_words("build_copper_planes ptrs", scratch.chip,
                           CHIP_COP_BPLPTRS + 2, g->chip,
                           CHIP_COP_BPLPTRS + 2, planes * 2, 4, 4);
    if (ptr_fail) {
        /* the dump may hold the OTHER buffer (verb ran for the back
         * frame after the swap); try the alternate before failing */
        uint32_t back = g32(g->base, G_SCREEN_B);
        fprintf(stderr, "  retrying with back buffer $%06x\n", back);
        build_copper_planes(&scratch, planes, back);
        ptr_fail = diff_words("build_copper_planes ptrs(back)", scratch.chip,
                              CHIP_COP_BPLPTRS + 2, g->chip,
                              CHIP_COP_BPLPTRS + 2, planes * 2, 4, 4);
    }
    status |= ptr_fail;

    free(scratch.chip);
    free(scratch.base);
    return status;
}

/* --verify-road: replay a snapshot-verified stage.  Loads the entry fast
 * image, runs the native port, and demands byte equality with the exit
 * image everywhere except the supervisor stack page (the BSR return
 * address the host pushed between the two snapshots). */
/* Entry registers for the stage under test, read from the snapshot's
 * .regs sibling.  Some routines take register inputs the caller set up
 * (the perspective pass inherits D3), so a standalone port has to be fed
 * the same values the game had. */
static uint32_t stage_d[8], stage_a[8];
static Blitter stage_blit;      /* chipset state from the snapshot */
static Input stage_input;       /* pad state from the snapshot */
/* Register results a stage produces.  Several routines return values in
 * registers (the scenery iterators hand back an advanced pointer), so a
 * port is only properly gated if those are checked against the exit
 * snapshot's regs as well as memory.  Index 0-7 = D0-D7, 8-15 = A0-A7. */
static uint32_t stage_out[16];
static int stage_out_used[16];
static void stage_expect(int reg, uint32_t value)
{ stage_out[reg] = value; stage_out_used[reg] = 1; }
static void load_regs(const char *entry_fast)
{
    char path[512];
    snprintf(path, sizeof path, "%s", entry_fast);
    char *tail = strstr(path, "_fast.bin");
    if (!tail) return;
    strcpy(tail, ".regs");
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "  (no regs: %s)\n", path); return; }
    char key[32];
    unsigned val;
    memset(&stage_blit, 0, sizeof stage_blit);
    memset(stage_out_used, 0, sizeof stage_out_used);
    memset(&stage_input, 0, sizeof stage_input);
    while (fscanf(f, "%31s %x", key, &val) == 2) {
        if (!strncmp(key, "blt", 3)) {
            if (!strcmp(key, "bltcon0")) stage_blit.bltcon0 = (uint16_t)val;
            else if (!strcmp(key, "bltcon1")) stage_blit.bltcon1 = (uint16_t)val;
            else if (!strcmp(key, "bltafwm")) stage_blit.bltafwm = (uint16_t)val;
            else if (!strcmp(key, "bltalwm")) stage_blit.bltalwm = (uint16_t)val;
            else if (!strncmp(key, "bltpt", 5) && key[5] >= '0' && key[5] <= '3')
                stage_blit.bltpt[key[5] - '0'] = val;
            else if (!strncmp(key, "bltmod", 6) && key[6] >= '0' && key[6] <= '3')
                stage_blit.bltmod[key[6] - '0'] = (int16_t)val;
            else if (!strncmp(key, "bltdat", 6) && key[6] >= '0' && key[6] <= '2')
                stage_blit.bltdat[key[6] - '0'] = (uint16_t)val;
        }
        else if (!strcmp(key, "joy0dat")) stage_input.joy0dat = (uint16_t)val;
        else if (!strcmp(key, "joy1dat")) stage_input.joy1dat = (uint16_t)val;
        else if (!strcmp(key, "ciapra")) stage_input.cia_pra = (uint8_t)val;
        else if (key[0] == 'd' && key[1] >= '0' && key[1] <= '7' && !key[2])
            stage_d[key[1] - '0'] = val;
        else if (key[0] == 'a' && key[1] >= '0' && key[1] <= '7' && !key[2])
            stage_a[key[1] - '0'] = val;
    }
    fclose(f);
}

/* swap "_fast.bin" for "_chip.bin" in a snapshot path */
static char *chip_path_of(const char *fast_path, char *buf, size_t n)
{
    snprintf(buf, n, "%s", fast_path);
    char *tail = strstr(buf, "_fast.bin");
    if (!tail) return NULL;
    strcpy(tail, "_chip.bin");
    return buf;
}

static int verify_stage(const char *name, const char *entry_fast,
                        const char *exit_fast,
                        void (*stage)(Game *g))
{
    Game g = {0};
    size_t len = 0;
    g.fast = guest_load(entry_fast, GUEST_FAST_SIZE, &len);
    uint8_t *want = guest_load(exit_fast, GUEST_FAST_SIZE, &len);
    if (!g.fast || !want) return 1;
    /* stages that build copper lists write chip RAM, so load it too */
    char cbuf[512], cbuf2[512];
    uint8_t *want_chip = NULL;
    if (chip_path_of(entry_fast, cbuf, sizeof cbuf) &&
        chip_path_of(exit_fast, cbuf2, sizeof cbuf2)) {
        g.chip = guest_load(cbuf, GUEST_CHIP_SIZE, &len);
        want_chip = guest_load(cbuf2, GUEST_CHIP_SIZE, &len);
    }
    g.base = g.fast + (GUEST_BASE_ADDR - GUEST_FAST_ADDR);
    load_regs(entry_fast);
    stage(&g);
    long bad = 0, shown = 0;
    for (uint32_t i = 0; i < GUEST_FAST_SIZE; i++) {
        uint32_t addr = GUEST_FAST_ADDR + i;
        if (addr >= 0x217c00 && addr < 0x217e00) continue;  /* stack page */
        if (g.fast[i] != want[i]) {
            if (shown++ < 8)
                fprintf(stderr, "  %s $%06x: native $%02x oracle $%02x\n",
                        name, addr, g.fast[i], want[i]);
            bad++;
        }
    }
    /* Registers the ORACLE changed but the port never declares.
     *
     * verify_stage only compares registers a stage announces with
     * stage_expect, so a port that reproduces memory perfectly and leaves
     * a register stale passes.  That is survivable while the port is only
     * ever called from a test, and fatal the moment it replaces the real
     * routine: road_blitqueue returns the blit-queue pointer in A4, the
     * C port returns void, and the caller resumed on a stale A4 until an
     * rts popped a corrupt address.  So: anything the routine changed and
     * the port does not model is reported here, and a stage carrying such
     * a register is not eligible to be a native override. */
    int unmodelled = 0;
    {
        char ebuf[512];
        snprintf(ebuf, sizeof ebuf, "%s", entry_fast);
        char *et = strstr(ebuf, "_fast.bin");
        char xbuf[512];
        snprintf(xbuf, sizeof xbuf, "%s", exit_fast);
        char *xt = strstr(xbuf, "_fast.bin");
        if (et && xt) {
            strcpy(et, ".regs");
            strcpy(xt, ".regs");
            uint32_t ein[16] = {0}, xout[16] = {0};
            for (int pass = 0; pass < 2; pass++) {
                FILE *rf = fopen(pass ? xbuf : ebuf, "r");
                if (!rf) continue;
                char k[32]; unsigned v;
                while (fscanf(rf, "%31s %x", k, &v) == 2) {
                    int idx = -1;
                    if (k[0] == 'd' && k[1] >= '0' && k[1] <= '7' && !k[2])
                        idx = k[1] - '0';
                    else if (k[0] == 'a' && k[1] >= '0' && k[1] <= '7' && !k[2])
                        idx = 8 + (k[1] - '0');
                    if (idx >= 0) (pass ? xout : ein)[idx] = v;
                }
                fclose(rf);
            }
            for (int i = 0; i < 16; i++) {
                if (i == 8 + 7 || i == 8 + 3) continue;  /* A7 stack, A3 base */
                if (ein[i] != xout[i] && !stage_out_used[i]) {
                    if (unmodelled++ == 0)
                        fprintf(stderr, "  %s UNMODELLED registers:", name);
                    fprintf(stderr, " %c%d", i < 8 ? 'd' : 'a', i & 7);
                }
            }
            if (unmodelled) fprintf(stderr, "\n");
        }
    }

    /* register outputs vs the exit snapshot's regs */
    {
        char rbuf[512];
        snprintf(rbuf, sizeof rbuf, "%s", exit_fast);
        char *rt = strstr(rbuf, "_fast.bin");
        if (rt) {
            strcpy(rt, ".regs");
            FILE *rf = fopen(rbuf, "r");
            if (rf) {
                char k[32];
                unsigned v;
                while (fscanf(rf, "%31s %x", k, &v) == 2) {
                    int idx = -1;
                    if (k[0] == 'd' && k[1] >= '0' && k[1] <= '7' && !k[2])
                        idx = k[1] - '0';
                    else if (k[0] == 'a' && k[1] >= '0' && k[1] <= '7' && !k[2])
                        idx = 8 + (k[1] - '0');
                    if (idx >= 0 && stage_out_used[idx] &&
                        stage_out[idx] != v) {
                        fprintf(stderr, "  %s %s: native $%08x oracle "
                                "$%08x\n", name, k, stage_out[idx], v);
                        bad++;
                    }
                }
                fclose(rf);
            }
        }
    }
    if (g.chip && want_chip)
        for (uint32_t i = 0; i < GUEST_CHIP_SIZE; i++)
            if (g.chip[i] != want_chip[i]) {
                if (shown++ < 8)
                    fprintf(stderr, "  %s chip $%06x: native $%02x "
                            "oracle $%02x\n", name, i, g.chip[i],
                            want_chip[i]);
                bad++;
            }
    fprintf(stderr, "%s: %s (%ld bytes differ)%s\n", name,
            bad ? "FAIL" : "EXACT", bad,
            unmodelled ? "  [not override-eligible]" : "");
    free(g.fast);
    free(want);
    free(g.chip);
    free(want_chip);
    return bad != 0;
}

static void stage_interpolate(Game *g) { road_interpolate(g, 0); }
/* $214344: lea $3054(a3),a4; bsr $214354 */
static void stage_band_bounds(Game *g) { road_band_bounds(g, A3_BASE + 0x3054); }
/* Composed chains.  Verifying routines one at a time proves each is
 * right; running the whole chain from one snapshot to a much later one
 * proves they compose -- that no routine depends on state a neighbour
 * was supposed to leave behind and does not. */
static void chain_car(Game *g)
{
    const uint32_t view = A3_BASE + 0x3054;
    car_update(g, view);                        /* $211e78 */
    car_checkpoint(g, view);                    /* $211e7c */
    car_clock(g, view);                         /* $211e80 */
    car_distance(g, view, NULL);                /* $211e84 */
    car_shape(g, view);                         /* $211e88 */
    car_tick(g, view);                          /* $211e94 */
}

static void chain_road(Game *g)
{
    road_sky(g);                                /* $212f12 */
    road_keyframes_near(g);                     /* $212f16 */
    road_interpolate(g, 0);                     /* $212f1a */
    road_band_bounds(g, A3_BASE + 0x3054);      /* $212f1e */
    /* $213416 inherits D3, and in the chain that is whatever
     * road_band_bounds left in it -- the clipped bottom edge, which it
     * also stored at view+$98.  Reading it back makes the dependency
     * explicit instead of relying on a register the caller happened to
     * still hold. */
    road_perspective_near(g, f16(g, A3_BASE + 0x3054 + 0x98));
    road_blitqueue(g);                          /* $212f2a, after the A4 load */
}

/* $212cea takes D0 from its caller (the frame's steering accumulator) */
static void stage_frame_begin(Game *g)
{ race_frame_begin(g, (uint16_t)stage_d[0]); }

/* $211770: the pad decoder, fed the snapshot's joystick state */
static void stage_input_read(Game *g) { input_read(g, &stage_input); }

/* $211e78: the car model, run on the near view block */
static void stage_car(Game *g) { car_update(g, A3_BASE + 0x3054); }
static void stage_checkpoint(Game *g) { car_checkpoint(g, A3_BASE + 0x3054); }
static void stage_clock(Game *g) { car_clock(g, A3_BASE + 0x3054); }
static void stage_distance(Game *g)
{
    uint32_t r[16];
    car_distance(g, A3_BASE + 0x3054, r);
    stage_expect(0, r[0]); stage_expect(1, r[1]);
}
static void stage_shape(Game *g) { car_shape(g, A3_BASE + 0x3054); }
static void stage_tick(Game *g) { car_tick(g, A3_BASE + 0x3054); }

/* the four scenery iterators, each handing back its advanced pointer */
static void stage_scen_a2(Game *g)
{ stage_expect(8 + 2, scen_next_a2(g, stage_a[2])); }
static void stage_scen_a0(Game *g)
{ stage_expect(8 + 0, scen_next_a0(g, stage_a[0])); }
static void stage_scen_table(Game *g) { scen_next_table(g); }
static void stage_scen_a1(Game *g)
{ stage_expect(8 + 1, scen_next_a1(g, stage_a[1])); }
/* $21508a: the scheduler head, which leaves A0 and A2 for the iterators */
static void stage_scen_prepare(Game *g)
{
    uint32_t a0 = 0, a2 = 0;
    scen_prepare(g, &a0, &a2);
    stage_expect(8 + 0, a0);
    stage_expect(8 + 2, a2);
}

/* $2169e0: the span filler and its two blit-queue emitters, driven with
 * the caller's registers from the snapshot. */
static void stage_span_fill(Game *g)
{
    Span s = { stage_d[0], stage_d[1], stage_d[2], stage_d[3],
               stage_d[4], stage_d[5], stage_d[6], stage_d[7],
               stage_a[0], stage_a[1], stage_a[2], stage_a[4] };
    span_fill(g, &s);
    stage_expect(0, s.d0); stage_expect(1, s.d1);
    stage_expect(2, s.d2); stage_expect(3, s.d3);
    stage_expect(4, s.d4); stage_expect(5, s.d5);
    stage_expect(6, s.d6); stage_expect(7, s.d7);
    stage_expect(8 + 2, s.a2); stage_expect(8 + 4, s.a4);
}

static void stage_scen_sort(Game *g)
{
    uint32_t r[8];
    stage_expect(8 + 1, scen_sort(g, r));
    for (int i = 0; i < 8; i++) stage_expect(i, r[i]);
}

static void stage_frame_latch(Game *g)
{ stage_expect(8 + 1, car_frame_latch(g)); }

static void stage_shape_ptr(Game *g)
{ stage_expect(8 + 1, scen_shape_ptr(g, stage_a[1], stage_d[6])); }

/* $216346 alone: the emit half, entered directly.  scen_project's own
 * gate only ever exercised the shadow path, which is why its emit
 * variants could be wrong and still pass. */
static void stage_emit(Game *g)
{
    Span s = { stage_d[0], stage_d[1], stage_d[2], stage_d[3],
               stage_d[4], stage_d[5], stage_d[6], stage_d[7],
               stage_a[0], stage_a[1], stage_a[2], stage_a[4] };
    scen_emit(g, &s);
    stage_expect(0, s.d0); stage_expect(1, s.d1);
    stage_expect(2, s.d2); stage_expect(3, s.d3);
    stage_expect(4, s.d4); stage_expect(5, s.d5);
    stage_expect(6, s.d6); stage_expect(7, s.d7);
    stage_expect(8 + 0, s.a0); stage_expect(8 + 1, s.a1);
    stage_expect(8 + 2, s.a2); stage_expect(8 + 4, s.a4);
}

/* $2160f2 -> $216346: project a scenery object and queue its blit. */
static void stage_project(Game *g)
{
    Span s = { stage_d[0], stage_d[1], stage_d[2], stage_d[3],
               stage_d[4], stage_d[5], stage_d[6], stage_d[7],
               stage_a[0], stage_a[1], stage_a[2], stage_a[4] };
    scen_project(g, &s);
    stage_expect(0, s.d0); stage_expect(1, s.d1);
    stage_expect(2, s.d2); stage_expect(3, s.d3);
    stage_expect(4, s.d4); stage_expect(5, s.d5);
    stage_expect(6, s.d6); stage_expect(7, s.d7);
    stage_expect(8 + 0, s.a0); stage_expect(8 + 1, s.a1);
    stage_expect(8 + 2, s.a2); stage_expect(8 + 4, s.a4);
}

/* $2133be: the band blitter, driven with the caller's registers and the
 * chipset state, both taken from the snapshot. */
static void stage_bands(Game *g)
{
    stage_blit.chip = g->chip;
    stage_blit.chip_size = GUEST_CHIP_SIZE;
    uint16_t d0 = road_bands(g, &stage_blit,
                             f32(g, 0x208000u + 0x2f8e),   /* A0 */
                             0x208000u - 0x2bd8,           /* A4 */
                             0x208000u - 0x4180,           /* A2 */
                             f16(g, 0x208000u + 0x30e4),   /* D1 */
                             f16(g, 0x208000u + 0x30ec),   /* D2 */
                             f16(g, 0x208000u + 0x30dc),   /* D4 */
                             f32(g, 0x208000u + 0x30d8),   /* D5 */
                             0x2c);                        /* D6 */
    p16(g->fast, 0x208000u + 0x2eb0 - GUEST_FAST_ADDR, d0);
}

/* $21337c inherits D3 from its caller */
static void stage_perspective(Game *g)
{ road_perspective_near(g, (uint16_t)stage_d[3]); }

int main(int argc, char **argv)
{
    const char *chip_path = "re/pipeline/gate_chip_02000.bin";
    const char *base_path = "re/pipeline/gate_base_02000.bin";
    const char *out = NULL;
    long coplist = -1;
    bool verbs = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--chip") && i + 1 < argc) chip_path = argv[++i];
        else if (!strcmp(argv[i], "--base") && i + 1 < argc) base_path = argv[++i];
        else if (!strcmp(argv[i], "--render") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--coplist") && i + 1 < argc)
            coplist = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--verify-verbs")) verbs = true;
        else if (!strcmp(argv[i], "--verify-road")) {
            int rc = verify_stage("road_interpolate",
                                  "re/pipeline/road/st_3_212f1a_fast.bin",
                                  "re/pipeline/road/st_4_212f1e_fast.bin",
                                  stage_interpolate);
            rc |= verify_stage("CHAIN car (6 routines)",
                               "re/pipeline/road/ph_0_211e78_fast.bin",
                               "re/pipeline/road/ph_6_211e98_fast.bin",
                               chain_car);
            rc |= verify_stage("CHAIN road (6 routines)",
                               "re/pipeline/road/st_1_212f12_fast.bin",
                               "re/pipeline/road/st_8_212f2e_fast.bin",
                               chain_road);
            rc |= verify_stage("race_frame_begin",
                               "re/pipeline/road/fr_0_212cea_fast.bin",
                               "re/pipeline/road/fr_1_212e3c_fast.bin",
                               stage_frame_begin);
            rc |= verify_stage("race_frame_publish",
                               "re/pipeline/road/fp_0_212e58_fast.bin",
                               "re/pipeline/road/fp_1_212e78_fast.bin",
                               race_frame_publish);
            rc |= verify_stage("car_frame_latch",
                               "re/pipeline/road/lt_0_211948_fast.bin",
                               "re/pipeline/road/lt_1_21194c_fast.bin",
                               stage_frame_latch);
            rc |= verify_stage("input_read",
                               "re/pipeline/road/in_0_21186a_fast.bin",
                               "re/pipeline/road/in_1_21186e_fast.bin",
                               stage_input_read);
            rc |= verify_stage("car_update",
                               "re/pipeline/road/ph_0_211e78_fast.bin",
                               "re/pipeline/road/ph_1_211e7c_fast.bin",
                               stage_car);
            rc |= verify_stage("car_checkpoint",
                               "re/pipeline/road/ph_1_211e7c_fast.bin",
                               "re/pipeline/road/ph_2_211e80_fast.bin",
                               stage_checkpoint);
            rc |= verify_stage("car_clock",
                               "re/pipeline/road/ph_2_211e80_fast.bin",
                               "re/pipeline/road/ph_3_211e84_fast.bin",
                               stage_clock);
            rc |= verify_stage("car_distance",
                               "re/pipeline/road/ph_3_211e84_fast.bin",
                               "re/pipeline/road/ph_4_211e88_fast.bin",
                               stage_distance);
            rc |= verify_stage("car_shape",
                               "re/pipeline/road/ph_4_211e88_fast.bin",
                               "re/pipeline/road/ph_5_211e94_fast.bin",
                               stage_shape);
            rc |= verify_stage("car_tick",
                               "re/pipeline/road/ph_5_211e94_fast.bin",
                               "re/pipeline/road/ph_6_211e98_fast.bin",
                               stage_tick);
            rc |= verify_stage("scen_emit ($216346 via $215498)",
                               "re/pipeline/road/em1_0_215498_fast.bin",
                               "re/pipeline/road/em1_1_21549c_fast.bin",
                               stage_emit);
            rc |= verify_stage("scen_emit ($216346 via $2154bc)",
                               "re/pipeline/road/em2_0_2154bc_fast.bin",
                               "re/pipeline/road/em2_1_2154c0_fast.bin",
                               stage_emit);
            rc |= verify_stage("scen_project ($2160f2+$216346)",
                               "re/pipeline/road/pj_0_215f00_fast.bin",
                               "re/pipeline/road/pj_1_215f04_fast.bin",
                               stage_project);
            rc |= verify_stage("scen_shape_ptr (FOREST: early-out only)",
                               "re/pipeline/road/sl_0_215f08_fast.bin",
                               "re/pipeline/road/sl_1_215f0c_fast.bin",
                               stage_shape_ptr);
            rc |= verify_stage("span_fill",
                               "re/pipeline/road/sp_0_2169dc_fast.bin",
                               "re/pipeline/road/sp_1_2169de_fast.bin",
                               stage_span_fill);
            rc |= verify_stage("scen_prepare",
                               "re/pipeline/road/sh_0_21508a_fast.bin",
                               "re/pipeline/road/sh_1_2151b4_fast.bin",
                               stage_scen_prepare);
            rc |= verify_stage("scen_next_a2",
                               "re/pipeline/road/sd_0_2151b4_fast.bin",
                               "re/pipeline/road/sd_1_2151b8_fast.bin",
                               stage_scen_a2);
            rc |= verify_stage("scen_next_a0",
                               "re/pipeline/road/sd_1_2151b8_fast.bin",
                               "re/pipeline/road/sd_2_2151bc_fast.bin",
                               stage_scen_a0);
            rc |= verify_stage("scen_next_table",
                               "re/pipeline/road/sd_2_2151bc_fast.bin",
                               "re/pipeline/road/sd_3_2151c0_fast.bin",
                               stage_scen_table);
            rc |= verify_stage("scen_sort",
                               "re/pipeline/road/sd_3_2151c0_fast.bin",
                               "re/pipeline/road/sd_4_2151c4_fast.bin",
                               stage_scen_sort);
            rc |= verify_stage("scen_next_a1",
                               "re/pipeline/road/sd_5_2151c8_fast.bin",
                               "re/pipeline/road/sd_6_2151cc_fast.bin",
                               stage_scen_a1);
            rc |= verify_stage("road_sky",
                               "re/pipeline/road/st_1_212f12_fast.bin",
                               "re/pipeline/road/st_2_212f16_fast.bin",
                               road_sky);
            rc |= verify_stage("road_keyframes",
                               "re/pipeline/road/st_2_212f16_fast.bin",
                               "re/pipeline/road/st_3_212f1a_fast.bin",
                               road_keyframes_near);
            rc |= verify_stage("road_band_bounds",
                               "re/pipeline/road/st_4_212f1e_fast.bin",
                               "re/pipeline/road/st_5_212f22_fast.bin",
                               stage_band_bounds);
            rc |= verify_stage("road_perspective",
                               "re/pipeline/road/st_5_212f22_fast.bin",
                               "re/pipeline/road/st_6_212f26_fast.bin",
                               stage_perspective);
            rc |= verify_stage("road_bands",
                               "re/pipeline/road/bl_0_2133be_fast.bin",
                               "re/pipeline/road/bl_1_2133c2_fast.bin",
                               stage_bands);
            rc |= verify_stage("road_blitqueue",
                               "re/pipeline/road/st_7_212f2a_fast.bin",
                               "re/pipeline/road/st_8_212f2e_fast.bin",
                               road_blitqueue);
            return rc;
        }
        else {
            fprintf(stderr,
                "usage: lotus2_native [--chip BIN] [--base BIN]\n"
                "       [--render OUT.ppm [--coplist ADDR]] [--verify-verbs]\n");
            return 2;
        }
    }

    Game g;
    g.chip = guest_load(chip_path, GUEST_CHIP_SIZE, NULL);
    g.base = guest_load(base_path, GUEST_BASE_SIZE, NULL);
    if (!g.chip || !g.base) return 1;

    int status = 0;
    if (verbs) status |= verify_verbs(&g);

    if (out) {
        uint32_t lists[2] = {CHIP_COPLIST_1, CHIP_COPLIST_2};
        int count = 2;
        if (coplist >= 0) { lists[0] = (uint32_t)coplist; count = 1; }
        for (int k = 0; k < count; k++) {
            if (composite(g.chip, lists[k], framebuf)) {
                fprintf(stderr, "no copper list at $%06x\n", lists[k]);
                status = 1;
                continue;
            }
            char path[512];
            if (count == 1) snprintf(path, sizeof path, "%s", out);
            else snprintf(path, sizeof path, "%s.%06x.ppm", out, lists[k]);
            status |= write_ppm_native(path, framebuf,
                                       LOTUS2_SCREEN_W, LOTUS2_SCREEN_H);
            fprintf(stderr, "lotus2_native: wrote %s (coplist $%06x)\n",
                    path, lists[k]);
        }
    }
    return status;
}
