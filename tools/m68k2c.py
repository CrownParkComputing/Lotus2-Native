#!/usr/bin/env python3
"""m68k2c -- static 68000 -> C recompiler for the Lotus 2 native port.

Route A of the fork recorded in the amiga_decomp_pipeline notes: instead
of rewriting each routine's meaning by hand, translate every executed
instruction mechanically and let the existing snapshot gates prove it.

Every bug found while hand-porting this game was an operand-width or
register-write mistake -- reading the low word of a long, treating `lea`
as a constant instead of a register store, modelling `move.b` as a word.
Those rules live in exactly one place here, so they are either right
everywhere or wrong everywhere (and the gates say which).

Input is `build/dasm` output over the combined image, filtered to the PCs
the game actually executes.  Output is one `switch (pc)` inside a
`for (;;)` loop, over the M68K struct in src/recomp/m68krt.h; `bsr`/`rts`
are ordinary pc writes against a real stack, so calls need no special
handling.

  python3 tools/m68k2c.py --pcset re/pipeline/pcset_race.txt \\
      --range 20d000-217200 --out src/recomp/lotus2_recomp.c
"""
import argparse, re, subprocess, sys

# ---------------------------------------------------------------- parsing

def split_operands(text):
    """Split on commas that are not inside parentheses."""
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch == '(': depth += 1
        elif ch == ')': depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip(): out.append(cur.strip())
    return out

def num(tok):
    """$1f / -$2 / 1f -> int"""
    tok = tok.strip()
    neg = tok.startswith('-')
    if neg: tok = tok[1:]
    tok = tok.lstrip('$')
    v = int(tok, 16)
    return -v if neg else v

class Insn:
    def __init__(self, addr, mnem, size, ops, length, text):
        self.addr, self.mnem, self.size = addr, mnem, size
        self.ops, self.length, self.text = ops, length, text
        self.next = addr + length

SIZES = {'b': 1, 'w': 2, 'l': 4, 's': 2}

def parse(lines):
    raw = []
    for line in lines:
        m = re.match(r'\$([0-9a-f]{6})\s+(\S+)\s*(.*)', line)
        if not m: continue
        addr = int(m.group(1), 16)
        mnem = m.group(2)
        rest = m.group(3)
        resolved = re.search(r';\s*\(\$([0-9a-f]+)\)', rest)
        rest = rest.split(';')[0].strip()
        size = None
        if '.' in mnem:
            mnem, sfx = mnem.rsplit('.', 1)
            size = SIZES.get(sfx)
        ops = split_operands(rest)
        if resolved:                      # PC-relative operand, pre-resolved
            for i, o in enumerate(ops):
                if o.endswith(',PC)'):
                    ops[i] = '$%06x' % int(resolved.group(1), 16)
        raw.append((addr, mnem, size, ops, line.rstrip()))
    out = []
    for i, (addr, mnem, size, ops, text) in enumerate(raw):
        nxt = raw[i + 1][0] if i + 1 < len(raw) else addr + 2
        out.append(Insn(addr, mnem, size, ops, nxt - addr, text))
    return out

# ------------------------------------------------------------- operands

DREG = re.compile(r'^D([0-7])$')
AREG = re.compile(r'^A([0-7])$')
IMM  = re.compile(r'^#(-?\$?[0-9a-f]+)$')
ABS  = re.compile(r'^(-?\$[0-9a-f]+)(?:\.[wl])?$')   # $1234.w / $dff000.l
IND  = re.compile(r'^\(A([0-7])\)$')
POST = re.compile(r'^\(A([0-7])\)\+$')
PRE  = re.compile(r'^-\(A([0-7])\)$')
D16  = re.compile(r'^\((-?\$?[0-9a-f]+),A([0-7])\)$')
IDX0 = re.compile(r'^\(A([0-7]),([DA])([0-7])\.([wl])\)$')
IDX  = re.compile(r'^\((-?\$?[0-9a-f]+),A([0-7]),([DA])([0-7])\.([wl])\)$')

class Op:
    """One operand, lowered to C fragments."""
    def __init__(self, kind, **kw):
        self.kind = kind
        self.__dict__.update(kw)

def lower(tok):
    m = DREG.match(tok)
    if m: return Op('d', r=int(m.group(1)))
    m = AREG.match(tok)
    if m: return Op('a', r=int(m.group(1)))
    m = IMM.match(tok)
    if m: return Op('imm', v=num(m.group(1)))
    m = IND.match(tok)
    if m: return Op('ind', r=int(m.group(1)))
    m = POST.match(tok)
    if m: return Op('post', r=int(m.group(1)))
    m = PRE.match(tok)
    if m: return Op('pre', r=int(m.group(1)))
    m = D16.match(tok)
    if m: return Op('d16', d=num(m.group(1)), r=int(m.group(2)))
    m = IDX.match(tok)
    if m: return Op('idx', d=num(m.group(1)), r=int(m.group(2)),
                    xt=m.group(3), xr=int(m.group(4)), xs=m.group(5))
    m = IDX0.match(tok)
    if m: return Op('idx', d=0, r=int(m.group(1)),
                    xt=m.group(2), xr=int(m.group(3)), xs=m.group(4))
    m = ABS.match(tok)
    if m: return Op('abs', v=num(m.group(1)))
    if tok in ('CCR', 'SR'): return Op('sr')
    if re.match(r'^[DA][0-7](-[DA][0-7])?(/[DA][0-7](-[DA][0-7])?)*$', tok):
        return Op('list', spec=tok)
    return Op('?', tok=tok)

def idx_expr(op):
    reg = ('m->d[%d]' % op.xr) if op.xt == 'D' else ('m->a[%d]' % op.xr)
    if op.xs == 'w':
        reg = '(uint32_t)(int32_t)(int16_t)%s' % reg
    return reg

_uniq = [0]
def fresh(tag):
    _uniq[0] += 1
    return '%s%d' % (tag, _uniq[0])

def ea(op, out, tag):
    """Emit the effective address into a temporary; return its name."""
    name = 'ea_%s' % fresh(tag)
    if op.kind == 'ind':
        out.append('uint32_t %s = m->a[%d];' % (name, op.r))
    elif op.kind == 'post':
        out.append('uint32_t %s = m->a[%d];' % (name, op.r))
    elif op.kind == 'pre':
        out.append('uint32_t %s;' % name)
    elif op.kind == 'd16':
        out.append('uint32_t %s = m->a[%d] + %d;' % (name, op.r, op.d))
    elif op.kind == 'idx':
        out.append('uint32_t %s = m->a[%d] + %d + %s;'
                   % (name, op.r, op.d, idx_expr(op)))
    elif op.kind == 'abs':
        out.append('uint32_t %s = 0x%x;' % (name, op.v & 0xffffffff))
    else:
        return None
    return name

MEMKINDS = ('ind', 'post', 'pre', 'd16', 'idx', 'abs')

def read_op(op, sz, out, tag):
    """Emit the read of `op`, returning a C expression."""
    if op.kind == 'd':   return 'm68k_trunc(m->d[%d], %d)' % (op.r, sz)
    if op.kind == 'a':   return 'm68k_trunc(m->a[%d], %d)' % (op.r, sz)
    if op.kind == 'imm': return '0x%xu' % (op.v & 0xffffffff)
    if op.kind in MEMKINDS:
        if op.kind == 'pre':
            out.append('m->a[%d] -= %d;' % (op.r, 2 if (op.r == 7 and sz == 1) else sz))
        n = ea(op, out, tag)
        if op.kind == 'pre':
            out.append('%s = m->a[%d];' % (n, op.r))
        vname = 'v_%s' % fresh(tag)
        out.append('uint32_t %s = m68k_rd(m, %s, %d);' % (vname, n, sz))
        if op.kind == 'post':
            out.append('m->a[%d] += %d;' % (op.r, 2 if (op.r == 7 and sz == 1) else sz))
        return vname
    return None

def write_op(op, sz, expr, out, tag):
    if op.kind == 'd':   out.append('m68k_setd(m, %d, %d, %s);' % (op.r, sz, expr)); return True
    if op.kind == 'a':   out.append('m68k_seta(m, %d, %d, %s);' % (op.r, sz, expr)); return True
    if op.kind in MEMKINDS:
        if op.kind == 'pre':
            out.append('m->a[%d] -= %d;' % (op.r, 2 if (op.r == 7 and sz == 1) else sz))
        n = ea(op, out, tag)
        if op.kind == 'pre':
            out.append('%s = m->a[%d];' % (n, op.r))
        out.append('m68k_wr(m, %s, %d, %s);' % (n, sz, expr))
        if op.kind == 'post':
            out.append('m->a[%d] += %d;' % (op.r, 2 if (op.r == 7 and sz == 1) else sz))
        return True
    return False

def reglist(spec):
    """'D0-D7/A0-A2' -> [(0..7 data), (8..10 addr)] as indices 0-15."""
    idx = []
    for part in spec.split('/'):
        m = re.match(r'^([DA])([0-7])(?:-([DA])([0-7]))?$', part)
        if not m: raise ValueError(spec)
        b = (0 if m.group(1) == 'D' else 8) + int(m.group(2))
        if m.group(3):
            e = (0 if m.group(3) == 'D' else 8) + int(m.group(4))
            idx.extend(range(b, e + 1))
        else:
            idx.append(b)
    return sorted(idx)

def regref(i):
    return 'm->d[%d]' % i if i < 8 else 'm->a[%d]' % (i - 8)

# ------------------------------------------------------------ emission

CC = {'ra': 't', 'eq': 'eq', 'ne': 'ne', 'cs': 'cs', 'cc': 'cc',
      'mi': 'mi', 'pl': 'pl', 'vs': 'vs', 'vc': 'vc', 'hi': 'hi',
      'ls': 'ls', 'ge': 'ge', 'lt': 'lt', 'gt': 'gt', 'le': 'le',
      't': 't'}

class Unsupported(Exception):
    pass

def emit(ins):
    """Return the C statements for one instruction (no pc update)."""
    o, sz, out = ins.ops, ins.size, []
    m = ins.mnem
    lo = [lower(x) for x in o]

    def src():  return read_op(lo[0], sz, out, 's')
    def dst_w(e): 
        if not write_op(lo[1], sz, e, out, 'd'): raise Unsupported(m)
    def dst_r(): 
        e = read_op(lo[1], sz, out, 'd')
        if e is None: raise Unsupported(m)
        return e

    if m == 'move' and len(lo) == 2 and lo[1].kind == 'sr':
        v = lo[0].v if lo[0].kind == 'imm' else 0
        out.append('m->c = %d; m->v = %d; m->z = %d; m->n = %d; m->x = %d;'
                   % (v & 1, (v >> 1) & 1, (v >> 2) & 1, (v >> 3) & 1,
                      (v >> 4) & 1))
        return out
    if m in ('move', 'movea'):
        s = src()
        if s is None: raise Unsupported(m)
        if lo[1].kind == 'a':
            out.append('m68k_seta(m, %d, %d, %s);' % (lo[1].r, sz, s))
        else:
            out.append('uint32_t r = %s;' % s)
            dst_w('r')
            if m == 'move':
                out.append('m68k_logic_flags(m, r, %d);' % sz)
        return out
    if m == 'moveq':
        out.append('m->d[%d] = 0x%xu;' % (lo[1].r, lo[0].v & 0xffffffff))
        out.append('m68k_logic_flags(m, m->d[%d], 4);' % lo[1].r)
        return out
    if m == 'lea':
        n = ea(lo[0], out, 's')
        if n is None: raise Unsupported(m)
        out.append('m->a[%d] = %s;' % (lo[1].r, n))
        return out
    if m == 'pea':
        n = ea(lo[0], out, 's')
        out.append('m68k_push32(m, %s);' % n)
        return out
    if m in ('add', 'addq', 'sub', 'subq') and len(lo) == 2 and lo[1].kind == 'a':
        # An as destination is ALWAYS a full 32-bit operation and sets no
        # flags, whatever the size suffix says -- this is `adda` in
        # disguise.  Getting it wrong loses the top half of the register
        # and is the single commonest 68k porting bug.
        sv = src()
        out.append('m->a[%d] %s= m68k_ext(%s, %d);'
                   % (lo[1].r, '+' if m.startswith('add') else '-', sv, sz))
        return out
    if m in ('cmp', 'cmpi') and len(lo) == 2 and lo[1].kind == 'a':
        sv = src()
        out.append('m68k_cmp_flags(m, m68k_ext(%s, %d), m->a[%d], 4);'
                   % (sv, sz, lo[1].r))
        return out
    if m in ('add', 'addi', 'addq', 'sub', 'subi', 'subq', 'cmp', 'cmpi',
             'and', 'andi', 'or', 'ori', 'eor', 'eori'):
        s = src()
        if s is None: raise Unsupported(m)
        out.append('uint32_t sv = %s;' % s)
        d = dst_r()
        out.append('uint32_t dv = %s;' % d)
        if m.startswith('add'):
            out.append('uint32_t r = m68k_add_flags(m, sv, dv, %d);' % sz)
            dst_w('r')
        elif m.startswith('sub'):
            out.append('uint32_t r = m68k_sub_flags(m, sv, dv, %d);' % sz)
            dst_w('r')
        elif m.startswith('cmp'):
            out.append('m68k_cmp_flags(m, sv, dv, %d);' % sz)
        else:
            op = {'and': '&', 'andi': '&', 'or': '|', 'ori': '|',
                  'eor': '^', 'eori': '^'}[m]
            out.append('uint32_t r = dv %s sv;' % op)
            dst_w('r')
            out.append('m68k_logic_flags(m, r, %d);' % sz)
        return out
    if m in ('adda', 'suba'):
        s = src()
        out.append('uint32_t sv = m68k_ext(%s, %d);' % (s, sz))
        out.append('m->a[%d] %s= sv;' % (lo[1].r, '+' if m == 'adda' else '-'))
        return out
    if m == 'cmpa':
        s = src()
        out.append('m68k_cmp_flags(m, m68k_ext(%s, %d), m->a[%d], 4);'
                   % (s, sz, lo[1].r))
        return out
    if m == 'cmpm':
        s = read_op(lo[0], sz, out, 's')
        d = read_op(lo[1], sz, out, 'd')
        out.append('m68k_cmp_flags(m, %s, %s, %d);' % (s, d, sz))
        return out
    if m in ('tst',):
        s = src()
        out.append('m68k_logic_flags(m, %s, %d);' % (s, sz))
        return out
    if m == 'clr':
        if not write_op(lo[0], sz, '0u', out, 's'): raise Unsupported(m)
        out.append('m->n = 0; m->z = 1; m->v = 0; m->c = 0;')
        return out
    if m in ('neg', 'not'):
        d = read_op(lo[0], sz, out, 's')
        out.append('uint32_t dv = %s;' % d)
        if m == 'neg':
            out.append('uint32_t r = m68k_sub_flags(m, dv, 0u, %d);' % sz)
        else:
            out.append('uint32_t r = m68k_trunc(~dv, %d);' % sz)
            out.append('m68k_logic_flags(m, r, %d);' % sz)
        if not write_op(lo[0], sz, 'r', out, 's2'): raise Unsupported(m)
        return out
    if m == 'ext':
        r = lo[0].r
        frm = 1 if sz == 2 else 2
        out.append('m68k_setd(m, %d, %d, m68k_ext(m68k_trunc(m->d[%d], %d), %d));'
                   % (r, sz, r, frm, frm))
        out.append('m68k_logic_flags(m, m->d[%d], %d);' % (r, sz))
        return out
    if m == 'swap':
        r = lo[0].r
        out.append('m->d[%d] = (m->d[%d] >> 16) | (m->d[%d] << 16);' % (r, r, r))
        out.append('m68k_logic_flags(m, m->d[%d], 4);' % r)
        return out
    if m == 'exg':
        out.append('{ uint32_t t = %s; %s = %s; %s = t; }'
                   % (regref(lo[0].r + (0 if lo[0].kind == 'd' else 8)),
                      regref(lo[0].r + (0 if lo[0].kind == 'd' else 8)),
                      regref(lo[1].r + (0 if lo[1].kind == 'd' else 8)),
                      regref(lo[1].r + (0 if lo[1].kind == 'd' else 8))))
        return out
    if m in ('mulu', 'muls'):
        s = src()
        r = lo[1].r
        if m == 'mulu':
            out.append('m->d[%d] = (uint32_t)((uint16_t)%s) * (uint32_t)(uint16_t)m->d[%d];'
                       % (r, s, r))
        else:
            out.append('m->d[%d] = (uint32_t)((int32_t)(int16_t)%s * (int32_t)(int16_t)m->d[%d]);'
                       % (r, s, r))
        out.append('m68k_logic_flags(m, m->d[%d], 4);' % r)
        return out
    if m in ('divu', 'divs'):
        s = src()
        r = lo[1].r
        out.append('{ uint32_t dv = %s;' % s)
        out.append('  if (dv == 0) { m->fault = "divide by zero"; m->halted = 1; break; }')
        if m == 'divu':
            out.append('  uint32_t q = m->d[%d] / (uint16_t)dv, rem = m->d[%d] %% (uint16_t)dv;' % (r, r))
            out.append('  if (q > 0xffff) { m->v = 1; }')
        else:
            out.append('  int32_t q = (int32_t)m->d[%d] / (int32_t)(int16_t)dv, rem = (int32_t)m->d[%d] %% (int32_t)(int16_t)dv;' % (r, r))
            out.append('  if (q > 32767 || q < -32768) { m->v = 1; }')
        # on overflow the 68000 leaves the destination UNCHANGED
        out.append('  else { m->v = 0; m->d[%d] = ((uint32_t)(uint16_t)rem << 16) | (uint16_t)q;' % r)
        out.append('         m->n = ((q >> 15) & 1); m->z = ((uint16_t)q == 0); }')
        out.append('  m->c = 0; }')
        return out
    if m in ('asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'roxl', 'roxr'):
        if len(lo) == 1:                       # memory shift by one
            d = read_op(lo[0], sz, out, 's')
            out.append('uint32_t dv = %s; uint32_t cnt = 1;' % d)
            target = None
        else:
            cnt = ('%du' % lo[0].v) if lo[0].kind == 'imm' \
                  else '(m68k_trunc(m->d[%d], 4) & 63)' % lo[0].r
            out.append('uint32_t cnt = %s;' % cnt)
            out.append('uint32_t dv = m68k_trunc(m->d[%d], %d);' % (lo[1].r, sz))
            target = lo[1].r
        bits = '%d' % (sz * 8)
        if m in ('asl', 'lsl'):
            out.append('uint32_t r = m68k_trunc(dv << cnt, %d);' % sz)
            out.append('m->c = m->x = cnt ? ((dv >> (%s - cnt)) & 1) : 0;' % bits)
        elif m == 'lsr':
            out.append('uint32_t r = m68k_trunc(dv >> cnt, %d);' % sz)
            out.append('m->c = m->x = cnt ? ((dv >> (cnt - 1)) & 1) : 0;')
        elif m == 'asr':
            out.append('uint32_t r = m68k_trunc((uint32_t)(m68k_ext(dv, %d) >> cnt), %d);' % (sz, sz))
            out.append('m->c = m->x = cnt ? ((dv >> (cnt - 1)) & 1) : 0;')
        elif m in ('rol', 'ror'):
            sh = 'cnt %% %s' % bits
            if m == 'rol':
                out.append('uint32_t s2 = %s; uint32_t r = m68k_trunc((dv << s2) | (dv >> (%s - s2)), %d);' % (sh, bits, sz))
                out.append('m->c = cnt ? (r & 1) : 0;')
            else:
                out.append('uint32_t s2 = %s; uint32_t r = m68k_trunc((dv >> s2) | (dv << (%s - s2)), %d);' % (sh, bits, sz))
                out.append('m->c = cnt ? m68k_msb(r, %d) : 0;' % sz)
        else:                                   # roxl / roxr, count 1 only
            if m == 'roxr':
                out.append('uint32_t r = m68k_trunc((dv >> 1) | ((uint32_t)m->x << (%s - 1)), %d);' % (bits, sz))
                out.append('m->c = m->x = (dv & 1);')
            else:
                out.append('uint32_t r = m68k_trunc((dv << 1) | (uint32_t)m->x, %d);' % sz)
                out.append('m->c = m->x = m68k_msb(dv, %d);' % sz)
        out.append('m->n = m68k_msb(r, %d); m->z = (m68k_trunc(r, %d) == 0); m->v = 0;' % (sz, sz))
        if target is None:
            if not write_op(lo[0], sz, 'r', out, 's2'): raise Unsupported(m)
        else:
            out.append('m68k_setd(m, %d, %d, r);' % (target, sz))
        return out
    if m in ('btst', 'bset', 'bclr', 'bchg'):
        bsz = 4 if lo[1].kind == 'd' else 1
        bit = ('%d' % lo[0].v) if lo[0].kind == 'imm' else 'm->d[%d]' % lo[0].r
        d = read_op(lo[1], bsz, out, 'd')
        out.append('uint32_t bn = (%s) %% %d;' % (bit, bsz * 8))
        out.append('uint32_t dv = %s;' % d)
        out.append('m->z = ((dv >> bn) & 1) == 0;')
        if m != 'btst':
            expr = {'bset': 'dv | (1u << bn)', 'bclr': 'dv & ~(1u << bn)',
                    'bchg': 'dv ^ (1u << bn)'}[m]
            out.append('uint32_t r = %s;' % expr)
            if not write_op(lo[1], bsz, 'r', out, 'd2'): raise Unsupported(m)
        return out
    if m == 'movem':
        regs = reglist(lo[0].spec if lo[0].kind == 'list' else lo[1].spec)
        if lo[0].kind == 'list':                # store
            if lo[1].kind == 'pre':             # predecrement: reverse order
                for i in reversed(regs):
                    out.append('m->a[%d] -= %d; m68k_wr(m, m->a[%d], %d, m68k_trunc(%s, %d));'
                               % (lo[1].r, sz, lo[1].r, sz, regref(i), sz))
            else:
                n = ea(lo[1], out, 'd')
                for k, i in enumerate(regs):
                    out.append('m68k_wr(m, %s + %d, %d, m68k_trunc(%s, %d));'
                               % (n, k * sz, sz, regref(i), sz))
        else:                                   # load
            if lo[0].kind == 'post':
                for i in regs:
                    out.append('%s = m68k_ext(m68k_rd(m, m->a[%d], %d), %d); m->a[%d] += %d;'
                               % (regref(i), lo[0].r, sz, sz, lo[0].r, sz))
            else:
                n = ea(lo[0], out, 's')
                for k, i in enumerate(regs):
                    out.append('%s = m68k_ext(m68k_rd(m, %s + %d, %d), %d);'
                               % (regref(i), n, k * sz, sz, sz))
        return out
    if m == 'addx':
        # X-chained add: Z is only CLEARED, never set, so a multiword
        # accumulate reports zero only when every word was zero.
        out.append('uint32_t sv = m68k_trunc(m->d[%d], %d);' % (lo[0].r, sz))
        out.append('uint32_t dv = m68k_trunc(m->d[%d], %d);' % (lo[1].r, sz))
        out.append('uint32_t xv = (uint32_t)m->x;')
        out.append('uint32_t r = m68k_trunc(dv + sv + xv, %d);' % sz)
        out.append('int sm = m68k_msb(sv, %d), dm = m68k_msb(dv, %d), rm = m68k_msb(r, %d);'
                   % (sz, sz, sz))
        out.append('m->v = (sm && dm && !rm) || (!sm && !dm && rm);')
        out.append('m->c = m->x = (sm && dm) || (!rm && (sm || dm));')
        out.append('m->n = rm; if (r != 0) m->z = 0;')
        out.append('m68k_setd(m, %d, %d, r);' % (lo[1].r, sz))
        return out
    if m == 'subx':
        out.append('uint32_t sv = m68k_trunc(m->d[%d], %d);' % (lo[0].r, sz))
        out.append('uint32_t dv = m68k_trunc(m->d[%d], %d);' % (lo[1].r, sz))
        out.append('uint32_t r = m68k_trunc(dv - sv - (uint32_t)m->x, %d);' % sz)
        out.append('int sm = m68k_msb(sv, %d), dm = m68k_msb(dv, %d), rm = m68k_msb(r, %d);'
                   % (sz, sz, sz))
        out.append('m->v = (!sm && dm && !rm) || (sm && !dm && rm);')
        out.append('m->c = m->x = (sm && !dm) || (rm && (sm || !dm));')
        out.append('m->n = rm; if (r != 0) m->z = 0;')
        out.append('m68k_setd(m, %d, %d, r);' % (lo[1].r, sz))
        return out
    if m == 'move' and len(lo) == 2 and lo[1].kind == 'sr':
        # move #imm,CCR -- only the low five bits matter here
        v = lo[0].v
        out.append('m->c = %d; m->v = %d; m->z = %d; m->n = %d; m->x = %d;'
                   % (v & 1, (v >> 1) & 1, (v >> 2) & 1, (v >> 3) & 1,
                      (v >> 4) & 1))
        return out
    if m == 'nop':
        return ['(void)0;']
    raise Unsupported(m)

def emit_case(ins, known):
    """Full case body including the pc update / control flow."""
    m, o = ins.mnem, ins.ops
    nxt = 'm->pc = 0x%x; break;' % ins.next
    body = []

    if m == 'rts':
        return ['m->pc = m68k_pop32(m); break;']
    if m == 'rte':
        return ['m->fault = "rte"; m->halted = 1; break;']
    if m in ('bsr', 'jsr'):
        t = lower(o[0])
        if t.kind == 'abs':
            return ['m68k_push32(m, 0x%x);' % ins.next,
                    'm->pc = 0x%x; break;' % (t.v & 0xffffff)]
        out = []
        n = ea(t, out, 's')
        if n is None:
            return ['m->fault = "jsr mode"; m->halted = 1; break;']
        out.append('m68k_push32(m, 0x%x);' % ins.next)
        out.append('m->pc = %s; break;' % n)
        return out
    if m in ('bra', 'jmp'):
        t = lower(o[0])
        if t.kind == 'abs':
            return ['m->pc = 0x%x; break;' % (t.v & 0xffffff)]
        out = []
        n = ea(t, out, 's')
        if n is None:
            return ['m->fault = "jmp mode"; m->halted = 1; break;']
        out.append('m->pc = %s; break;' % n)
        return out
    if m.startswith('b') and m[1:] in CC and m not in ('bset', 'bclr', 'bchg', 'btst'):
        t = lower(o[0])
        return ['if (m68k_cc_%s(m)) { m->pc = 0x%x; break; }' % (CC[m[1:]], t.v & 0xffffff),
                nxt]
    if m.startswith('db'):
        t = lower(o[1])
        r = lower(o[0]).r
        cc = m[2:]
        pre = ''
        if cc not in ('ra', 'f', ''):
            pre = 'if (m68k_cc_%s(m)) { %s }\n        ' % (CC.get(cc, 't'), nxt)
        return [pre + '{ uint16_t cnt = (uint16_t)(m68k_trunc(m->d[%d], 2) - 1);' % r,
                '  m68k_setd(m, %d, 2, cnt);' % r,
                '  if (cnt != 0xffff) { m->pc = 0x%x; break; } }' % (t.v & 0xffffff),
                nxt]
    try:
        body = emit(ins)
    except (Unsupported, ValueError, AttributeError, TypeError) as exc:
        return ['m->fault = "%s @ $%06x: %s"; m->halted = 1; break;'
                % (type(exc).__name__, ins.addr,
                   ins.text.strip().replace('"', "'")[:60])]
    body.append(nxt)
    return body

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', default='re/pipeline/combined.bin')
    ap.add_argument('--pcset', help='only translate PCs listed here')
    ap.add_argument('--range', default='20d000-217200')
    ap.add_argument('--out', default='src/recomp/lotus2_recomp.c')
    a = ap.parse_args()

    lo, hi = (int(x, 16) for x in a.range.split('-'))
    text = subprocess.run(['./build/dasm', a.image, hex(lo), hex(hi)],
                          capture_output=True, text=True).stdout
    insns = parse(text.splitlines())
    keep = None
    if a.pcset:
        keep = set(int(l.strip(), 16) for l in open(a.pcset) if l.strip())
        insns = [i for i in insns if i.addr in keep]

    known = set(i.addr for i in insns)
    cases, faults = [], 0
    for ins in insns:
        body = emit_case(ins, known)
        if any('m->fault =' in b and 'divide' not in b for b in body):
            faults += 1
        # each case gets its own block: the temporaries are per-instruction
        cases.append('    case 0x%06x: {  /* %s */\n%s\n    }'
                     % (ins.addr, ins.text.split(None, 1)[1].strip(),
                        '\n'.join('        ' + b for b in body)))

    with open(a.out, 'w') as f:
        f.write('/* GENERATED by tools/m68k2c.py -- do not edit.\n'
                ' *\n'
                ' * %d instructions from %s, range $%06x-$%06x.\n'
                ' * Operand widths and register-write rules come from the\n'
                ' * generator and src/recomp/m68krt.h, so they are uniform\n'
                ' * across every routine instead of re-derived per hand-port.\n'
                ' */\n' % (len(insns), a.pcset or 'all PCs', lo, hi))
        f.write('#include "m68krt.h"\n\n')
        f.write('void lotus2_recomp_run(M68K *m, uint32_t stop_pc)\n{\n')
        f.write('    while (!m->halted && m->pc != stop_pc) {\n')
        f.write('    switch (m->pc) {\n')
        f.write('\n'.join(cases))
        f.write('\n    default:\n'
                '        m->fault = "untranslated pc";\n'
                '        m->halted = 1;\n'
                '        break;\n'
                '    }\n    }\n}\n')
    sys.stderr.write('m68k2c: %d instructions, %d untranslated -> %s\n'
                     % (len(insns), faults, a.out))

if __name__ == '__main__':
    main()
