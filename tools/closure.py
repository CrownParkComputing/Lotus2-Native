#!/usr/bin/env python3
"""Call-graph closure over build/dasm output.

Usage: python3 tools/closure.py   (roots are set at the bottom)

Walks the CFG from each root, follows BSR/JSR targets, and reports how
many functions and instructions a subsystem really spans.  The
next-instruction length is inferred from addresses already in the map,
so the edge list can over-approximate where the map has stale entries --
trust the function set and the LEAF set, treat edges as a hint.
"""
import subprocess, re, sys, os
IMG='re/pipeline/combined.bin'
def dis(lo,hi):
    out=subprocess.run(['./build/dasm',IMG,hex(lo),hex(hi)],capture_output=True,text=True).stdout
    m={}
    for line in out.splitlines():
        mm=re.match(r'\$([0-9a-f]{6})\s+(\S+)\s*(.*)',line)
        if mm: m[int(mm.group(1),16)]=(mm.group(2),mm.group(3).strip())
    return m
MAP={}
def ensure(a):
    if a not in MAP:
        MAP.update(dis(a, a+0x600))
    return MAP.get(a)

TERM={'rts','rte','rtr','jmp'}
UNCOND={'bra','bra.b','bra.w'}
def walk(entry, seen_i, calls):
    stack=[entry]; local=set()
    while stack:
        pc=stack.pop()
        while True:
            if pc in local: break
            ins=ensure(pc)
            if ins is None: break
            local.add(pc); seen_i.add(pc)
            op,arg=ins
            base=op.split('.')[0]
            # instruction length: next known address
            nxt=None
            for d in (2,4,6,8,10,12):
                if pc+d in MAP: nxt=pc+d; break
            if base in ('bsr','jsr'):
                t=re.match(r'\$([0-9a-f]+)',arg)
                if t: calls.add(int(t.group(1),16))
            if base in TERM: break
            if base.startswith('b') and base not in ('bset','bclr','bchg','btst','bfext'):
                t=re.match(r'\$([0-9a-f]+)',arg)
                if t: stack.append(int(t.group(1),16))
                if base=='bra': break
            if base=='dbra' or base.startswith('db'):
                t=re.match(r'.*\$([0-9a-f]+)',arg)
                if t: stack.append(int(t.group(1),16))
            if nxt is None: break
            pc=nxt

roots=[0x21508a]
done=set(); pend=set(roots); allins=set()
while pend:
    f=pend.pop(); done.add(f)
    calls=set(); walk(f, allins, calls)
    for c in calls:
        if c not in done: pend.add(c)
print("functions in closure:", len(done))
print("distinct instructions:", len(allins))
PORTED={0x215a7a,0x215a9c,0x215adc,0x215b24,0x215b58,0x21508a}
print("already ported roots:", len(PORTED & done))
for f in sorted(done): print(hex(f), end=' ')
print()
