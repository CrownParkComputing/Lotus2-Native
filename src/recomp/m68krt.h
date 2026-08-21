/* m68krt.h -- runtime for statically recompiled 68000 code.
 *
 * The generator (tools/m68k2c.py) turns each executed instruction into a
 * `case` of a `switch (pc)` inside a `for (;;)` loop, operating on this
 * machine struct.  Because the stack is real memory and `bsr`/`rts` are
 * ordinary pc writes, subroutine calls need no special handling: to run
 * one routine you push a sentinel return address and spin until the pc
 * comes back to it.
 *
 * The point of generating rather than hand-porting is that operand width
 * and register-write semantics are encoded ONCE, here and in the
 * generator, instead of being re-derived (and re-fumbled) per routine:
 *   - `move.w` into a data register preserves the upper 16 bits
 *   - `moveq`, `move.l` and every `lea` write all 32
 *   - `adda.w` sign-extends its source and adds to the WHOLE address reg
 *   - `move.b` writes exactly 8 bits
 * Those four rules account for every bug found while hand-porting.
 */
#ifndef M68KRT_H
#define M68KRT_H

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct M68K M68K;
struct M68K {
    uint32_t d[8], a[8];
    uint32_t pc;
    int n, z, v, c, x;          /* condition codes, kept as 0/1 */
    uint8_t *chip;              /* $000000..CHIP_SIZE */
    uint8_t *fast;              /* $200000.. */
    uint32_t chip_size, fast_size;
    uint16_t custom[256];       /* $dff000 writes land here */
    unsigned long cycles;       /* 68000 cycles consumed, measured not guessed */
    unsigned long cycles_base;  /* value at the start of this timeslice */
    uint16_t sr_hi;             /* SR bits above the CCR: S, interrupt mask */
    int irq_level;              /* pending autovector level, 0 = none */
    int halted;
    const char *fault;          /* set when an unimplemented op is hit */
};

/* ---- memory ----
 *
 * Generated code never touches memory directly: it goes through these two
 * hooks, which the surrounding program provides.  In the native build
 * they forward to the host's chipset layer, so a write to a custom
 * register still starts a blit or fetches a copper instruction.  In
 * recomp_verify they forward to flat snapshot images.  Same generated
 * code, judged the same way, in both.
 */
uint32_t m68krt_read(uint32_t addr, int size);
void     m68krt_write(uint32_t addr, int size, uint32_t value);

static inline uint32_t m68k_rd(M68K *m, uint32_t a, int sz)
{ (void)m; return m68krt_read(a, sz); }
static inline void m68k_wr(M68K *m, uint32_t a, int sz, uint32_t v)
{ (void)m; m68krt_write(a, sz, v); }

/* ---- register writes at each width ---- */
static inline void m68k_setd(M68K *m, int r, int sz, uint32_t v)
{
    if (sz == 1)      m->d[r] = (m->d[r] & 0xffffff00u) | (v & 0xff);
    else if (sz == 2) m->d[r] = (m->d[r] & 0xffff0000u) | (v & 0xffff);
    else              m->d[r] = v;
}
/* address registers are never partially written: a word operand is
 * sign-extended to 32 bits first */
static inline void m68k_seta(M68K *m, int r, int sz, uint32_t v)
{
    m->a[r] = (sz == 2) ? (uint32_t)(int32_t)(int16_t)v : v;
}

static inline uint32_t m68k_ext(uint32_t v, int sz)
{
    if (sz == 1) return (uint32_t)(int32_t)(int8_t)v;
    if (sz == 2) return (uint32_t)(int32_t)(int16_t)v;
    return v;
}
static inline uint32_t m68k_trunc(uint32_t v, int sz)
{
    if (sz == 1) return v & 0xff;
    if (sz == 2) return v & 0xffff;
    return v;
}
static inline int m68k_msb(uint32_t v, int sz)
{
    return (int)((v >> (sz * 8 - 1)) & 1);
}

/* ---- flags ---- */
static inline void m68k_logic_flags(M68K *m, uint32_t r, int sz)
{
    r = m68k_trunc(r, sz);
    m->n = m68k_msb(r, sz); m->z = (r == 0); m->v = 0; m->c = 0;
}
static inline uint32_t m68k_add_flags(M68K *m, uint32_t s, uint32_t d, int sz)
{
    uint32_t r = m68k_trunc(d + s, sz);
    int sm = m68k_msb(s, sz), dm = m68k_msb(d, sz), rm = m68k_msb(r, sz);
    m->n = rm; m->z = (r == 0);
    m->v = (sm && dm && !rm) || (!sm && !dm && rm);
    m->c = (sm && dm) || (!rm && (sm || dm));
    m->x = m->c;
    return r;
}
/* d - s */
static inline uint32_t m68k_sub_flags(M68K *m, uint32_t s, uint32_t d, int sz)
{
    uint32_t r = m68k_trunc(d - s, sz);
    int sm = m68k_msb(s, sz), dm = m68k_msb(d, sz), rm = m68k_msb(r, sz);
    m->n = rm; m->z = (r == 0);
    m->v = (!sm && dm && !rm) || (sm && !dm && rm);
    m->c = (sm && !dm) || (rm && (sm || !dm));
    m->x = m->c;
    return r;
}
/* cmp: like sub but leaves X alone */
static inline void m68k_cmp_flags(M68K *m, uint32_t s, uint32_t d, int sz)
{
    int x = m->x;
    m68k_sub_flags(m, s, d, sz);
    m->x = x;
}

/* ---- condition tests ---- */
static inline int m68k_cc_t (M68K *m) { (void)m; return 1; }
static inline int m68k_cc_f (M68K *m) { (void)m; return 0; }
static inline int m68k_cc_eq(M68K *m) { return m->z; }
static inline int m68k_cc_ne(M68K *m) { return !m->z; }
static inline int m68k_cc_cs(M68K *m) { return m->c; }
static inline int m68k_cc_cc(M68K *m) { return !m->c; }
static inline int m68k_cc_mi(M68K *m) { return m->n; }
static inline int m68k_cc_pl(M68K *m) { return !m->n; }
static inline int m68k_cc_vs(M68K *m) { return m->v; }
static inline int m68k_cc_vc(M68K *m) { return !m->v; }
static inline int m68k_cc_hi(M68K *m) { return !m->c && !m->z; }
static inline int m68k_cc_ls(M68K *m) { return m->c || m->z; }
static inline int m68k_cc_ge(M68K *m) { return m->n == m->v; }
static inline int m68k_cc_lt(M68K *m) { return m->n != m->v; }
static inline int m68k_cc_gt(M68K *m) { return (m->n == m->v) && !m->z; }
static inline int m68k_cc_le(M68K *m) { return (m->n != m->v) || m->z; }

/* ---- status register ---- */
static inline uint16_t m68k_get_sr(const M68K *m)
{
    return (uint16_t)(m->sr_hi | (m->x << 4) | (m->n << 3) |
                      (m->z << 2) | (m->v << 1) | m->c);
}
static inline void m68k_set_sr(M68K *m, uint16_t v)
{
    m->sr_hi = (uint16_t)(v & 0xffe0);
    m->x = (v >> 4) & 1; m->n = (v >> 3) & 1;
    m->z = (v >> 2) & 1; m->v = (v >> 1) & 1; m->c = v & 1;
}

static inline void m68k_push32(M68K *m, uint32_t v)
{ m->a[7] -= 4; m68k_wr(m, m->a[7], 4, v); }
static inline uint32_t m68k_pop32(M68K *m)
{ uint32_t v = m68k_rd(m, m->a[7], 4); m->a[7] += 4; return v; }
static inline void m68k_push16(M68K *m, uint16_t v)
{ m->a[7] -= 2; m68k_wr(m, m->a[7], 2, v); }
static inline uint16_t m68k_pop16(M68K *m)
{ uint16_t v = (uint16_t)m68k_rd(m, m->a[7], 2); m->a[7] += 2; return v; }

/* rte: the 68000 exception frame is SR then PC, pushed in that order.
 *
 * NOTE: Musashi re-checks interrupts here, via m68ki_set_sr ending in
 * m68ki_check_interrupts.  Modelling that -- together with treating the
 * IRQ as a level that stays asserted rather than an event consumed on
 * delivery -- was tried and made agreement WORSE (storm 17/25 against
 * 23/25).  The host raises and lowers the line at its own points, so the
 * two effects do not compose the way they do inside Musashi.  Left as it
 * is, deliberately, with the experiment recorded so it is not repeated.
 */
static inline void m68k_rte(M68K *m)
{
    m68k_set_sr(m, m68k_pop16(m));
    m->pc = m68k_pop32(m);
}

/* Take a pending autovectored interrupt.  Levels 2, 3 and 5 are the ones
 * this game arms (L2 $20f5aa, L3 VERTB $20f6c0, L5 BLIT $20d91a); the
 * vector is 24 + level, i.e. $68 / $6c / $74. */
static inline int m68k_take_irq(M68K *m, int level)
{
    int mask = (m->sr_hi >> 8) & 7;
    if (level == 0) return 0;
    if (level != 7 && level <= mask) return 0;
    uint16_t old = m68k_get_sr(m);
    m->sr_hi = (uint16_t)((m->sr_hi & ~0x0700u) | 0x2000u | (level << 8));
    m68k_push32(m, m->pc);
    m68k_push16(m, old);
    m->pc = m68k_rd(m, (uint32_t)(24 + level) * 4, 4);
    /* Exception overhead, charged here rather than smeared into the
     * interrupted instruction's average cost. */
    m->cycles += 44;
    return 1;
}

#endif
