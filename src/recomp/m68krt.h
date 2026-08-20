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
    int halted;
    const char *fault;          /* set when an unimplemented op is hit */
};

/* ---- memory ---- */
static inline uint8_t *m68k_host(M68K *m, uint32_t a)
{
    if (a < m->chip_size) return m->chip + a;
    if (a >= 0x200000u && a < 0x200000u + m->fast_size)
        return m->fast + (a - 0x200000u);
    return NULL;
}
static inline uint32_t m68k_rd(M68K *m, uint32_t a, int sz)
{
    if ((a & 0xfff000u) == 0xdff000u)
        return m->custom[(a & 0x1fe) >> 1];
    uint8_t *p = m68k_host(m, a);
    if (!p) return 0;
    if (sz == 1) return p[0];
    if (sz == 2) return ((uint32_t)p[0] << 8) | p[1];
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static inline void m68k_wr(M68K *m, uint32_t a, int sz, uint32_t v)
{
    if ((a & 0xfff000u) == 0xdff000u) {
        m->custom[(a & 0x1fe) >> 1] = (uint16_t)v;
        return;
    }
    uint8_t *p = m68k_host(m, a);
    if (!p) return;
    if (sz == 1) { p[0] = (uint8_t)v; return; }
    if (sz == 2) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return; }
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

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

static inline void m68k_push32(M68K *m, uint32_t v)
{ m->a[7] -= 4; m68k_wr(m, m->a[7], 4, v); }
static inline uint32_t m68k_pop32(M68K *m)
{ uint32_t v = m68k_rd(m, m->a[7], 4); m->a[7] += 4; return v; }

#endif
