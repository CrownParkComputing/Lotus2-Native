/* native_overrides.c -- run hand-written native C instead of the game's
 * instructions, one routine at a time.
 *
 * This is how the port stops being a recompilation and becomes native.
 * src/engine/ already holds C versions of the car model, the road
 * pipeline and the scenery pass, each proven byte-exact against the
 * oracle by a snapshot gate.  Registering one here makes the CPU skip
 * the game's instructions for that routine and call the C instead, and
 * `make frame-gate` says whether the whole game still matches.
 *
 * Two things make a swap honest:
 *
 *   The stack.  Each routine was entered by BSR, so the override finishes
 *   with the RTS the instructions would have done: pop the return address
 *   into the pc.  Registers the routine returns are written back.
 *
 *   The clock.  Native C costs nothing, and a CPU that gets its work done
 *   early is a CPU that is ahead of the chipset -- which is exactly the
 *   drift that used to send this game down a different path.  Each
 *   override therefore charges what the instructions charged, measured
 *   per call from the oracle with SWIV_RTNCYC rather than estimated.
 */
#include <string.h>
#include "amiga.h"
#include "m68krt.h"
#include "engine.h"

#define A3 0x208000u
#define VIEW (A3 + 0x3054)

/* the engine reads guest memory through the same windows the host uses */
static Game game_view(void)
{
    Game g = {0};
    g.chip = chip;
    g.fast = fast;
    g.base = fast + (GUEST_BASE_ADDR - GUEST_FAST_ADDR);
    return g;
}

static void ret(M68K *m, unsigned cycles)
{
    m->cycles += cycles;
    m->pc = m68k_pop32(m);
}

/* ---- the car model ($211e74's chain) ---- */
static int ov_car_update(M68K *m)
{ Game g = game_view(); car_update(&g, m->a[4]); ret(m, 970); return 1; }
static int ov_car_checkpoint(M68K *m)
{ Game g = game_view(); car_checkpoint(&g, m->a[4]); ret(m, 189); return 1; }
static int ov_car_clock(M68K *m)
{ Game g = game_view(); car_clock(&g, m->a[4]); ret(m, 107); return 1; }
static int ov_car_distance(M68K *m)
{ Game g = game_view(); car_distance(&g, m->a[4]); ret(m, 149); return 1; }
static int ov_car_shape(M68K *m)
{ Game g = game_view(); car_shape(&g, m->a[4]); ret(m, 461); return 1; }
static int ov_car_tick(M68K *m)
{ Game g = game_view(); car_tick(&g, m->a[4]); ret(m, 1564); return 1; }
static int ov_car_frame_latch(M68K *m)
{ Game g = game_view(); car_frame_latch(&g); ret(m, 904); return 1; }

/* ---- the scenery iterators ---- */
static int ov_scen_next_a2(M68K *m)
{ Game g = game_view(); m->a[2] = scen_next_a2(&g, m->a[2]); ret(m, 166); return 1; }
static int ov_scen_next_a0(M68K *m)
{ Game g = game_view(); m->a[0] = scen_next_a0(&g, m->a[0]); ret(m, 553); return 1; }
static int ov_scen_next_table(M68K *m)
{ Game g = game_view(); scen_next_table(&g); ret(m, 54); return 1; }
static int ov_scen_next_a1(M68K *m)
{ Game g = game_view(); m->a[1] = scen_next_a1(&g, m->a[1]); ret(m, 148); return 1; }
static int ov_scen_sort(M68K *m)
{ Game g = game_view(); scen_sort(&g); ret(m, 410); return 1; }

/* NOT overridable yet: road_blitqueue ($2143c2) and road_band_bounds
 * ($214354).
 *
 * Both leave results in registers -- road_blitqueue walks A4 along the
 * blit queue and returns it advanced, along with A0/D0/D1/D6/D7 -- and
 * the C ports take a Game and return void, so the caller would resume on
 * stale registers.  Overriding road_blitqueue did exactly that: an rts
 * eventually popped a corrupted address and the pc landed on $000000.
 *
 * The engine gate did not catch it because verify_stage only compares the
 * registers a stage DECLARES via stage_expect, and these declared none.
 * A routine may only be overridden once its register effects are
 * reproduced -- memory equality is not enough.
 */

/* ---- the scenery span filler ---- */
static int ov_span_fill(M68K *m)
{
    Game g = game_view();
    Span s = { m->d[0], m->d[1], m->d[2], m->d[3], m->d[4], m->d[5],
               m->d[6], m->d[7], m->a[0], m->a[1], m->a[2], m->a[4] };
    span_fill(&g, &s);
    m->d[0] = s.d0; m->d[1] = s.d1; m->d[2] = s.d2; m->d[3] = s.d3;
    m->d[4] = s.d4; m->d[5] = s.d5; m->d[6] = s.d6; m->d[7] = s.d7;
    m->a[0] = s.a0; m->a[1] = s.a1; m->a[2] = s.a2; m->a[4] = s.a4;
    ret(m, 1022);
    return 1;
}

typedef int (*NativeFn)(M68K *m);
static struct { uint32_t pc; NativeFn fn; const char *name; } table[] = {
    { 0x2129f2, ov_car_update,      "car_update" },
    { 0x212680, ov_car_checkpoint,  "car_checkpoint" },
    { 0x21263c, ov_car_clock,       "car_clock" },
    { 0x212662, ov_car_distance,    "car_distance" },
    { 0x212ba4, ov_car_shape,       "car_shape" },
    { 0x21270a, ov_car_tick,        "car_tick" },
    { 0x211dd4, ov_car_frame_latch, "car_frame_latch" },
    { 0x215a7a, ov_scen_next_a2,    "scen_next_a2" },
    { 0x215a9c, ov_scen_next_a0,    "scen_next_a0" },
    { 0x215adc, ov_scen_next_table, "scen_next_table" },
    { 0x215b24, ov_scen_next_a1,    "scen_next_a1" },
    { 0x215b58, ov_scen_sort,       "scen_sort" },
    { 0x2169e0, ov_span_fill,       "span_fill" },
};
#define NOVERRIDE ((int)(sizeof table / sizeof table[0]))

/* one bit per even address: rejecting a pc must cost nothing, because
 * this is consulted before every instruction */
static uint8_t present[1u << 21];
static int enabled;

static int limit = NOVERRIDE;

const char *native_override_name(int i)
{ return (i >= 0 && i < NOVERRIDE) ? table[i].name : "?"; }

/* LOTUS2_NATIVE=N enables only the first N routines, so a bad swap can
 * be bisected instead of guessed at. */
void native_overrides_init(int on)
{
    enabled = on > 0;
    limit = on > NOVERRIDE ? NOVERRIDE : on;
    memset(present, 0, sizeof present);
    if (!enabled) return;
    for (int i = 0; i < limit; i++) {
        uint32_t k = table[i].pc >> 1;
        present[k >> 3] |= (uint8_t)(1u << (k & 7));
    }
}

int native_overrides_count(void) { return enabled ? limit : 0; }

int native_override_try(M68K *m)
{
    uint32_t k = m->pc >> 1;
    if (!enabled || m->pc >= (1u << 24)) return 0;
    if (!(present[k >> 3] & (1u << (k & 7)))) return 0;
    for (int i = 0; i < limit; i++)
        if (table[i].pc == m->pc) return table[i].fn(m);
    return 0;
}
