# Lotus 2 phase-direct verb library — C pseudocode specification

Companion to `ARCH.md`.  "Verb" here = one of the fixed service routines the
sequencer/IRQ path calls (there is no gfx→handler table in this engine).
Addresses are runtime addresses in the combined image (fast base $200000,
chip base $0).  `g<off>` = base-page global at A3+off (A3 = $208000).
Status: **[verified]** = checked against the register trace;
**[contract]** = signature + effect known, body not yet line-ported.

## Conventions

* Frame tick: VBLANK (level 3) sets `g2fee`; every park loop spins on it.
  `g2ff0` nonzero anywhere = abort the current sequence (exit to $20e1c0).
* Phase = `g2fe0` plus params `g2fe8/$2fea/$2fec`; the frame phases
  alternate $11/$13 (double-buffered phases), sequence steps seen: $10,
  $18, $34.
* Screens: `g2fd6`/`g2fda` hold the two chip buffers; swap per phase.
* All coordinates are PAL 320x200; bitplane stride $1f40 (8000 bytes).

## 1. Frame synchronisation

### park loop ($20df86 / $20dfd6 / $20e15c / $20e1ac) [verified]
```c
void park(void) {                 // spin bodies; entry tails differ per site
    while (!g2fee)                // $20df8e: tst.w $2fee(a3); beq
        if (g2ff0) longjmp(exit); // $20df86: tst.w $2ff0(a3); bne $20e1c0
    g2fee = 0;                    // clr.w $2fee(a3) before each park
}
```

### $20d2a0 service_call(void) [verified]
`jsr $204.w` trampoline into the decrunched install body.  The slave's
install exports a per-call service at $204 (WHDLoad-facing helper).

### $20eab8 wait_raster(d1) [verified]
Busy-wait on VHPOSR ($dff006) sign edges, d1+1 transitions.  Delay verb
used with d1 = 5 / $a.

### $212c9e wait_blit_queue(void) [verified]
NOP-sled spin until `g2fa4 - g2fa8 >= 2` (blitter queue has room),
toggling INTENA $0002 around the check.  90k iterations in 80 gameplay
frames — this is the frame pacer during racing.

## 2. Per-frame display verbs

### $20f69e swap_screens(void) [verified]
```c
void swap_screens(void) { swap(g2fd6, g2fda); }   // 6 instructions
```

### $2102ca build_fade(void) [verified]
Rebuild the 32-colour fade target table at $5400 from the current
palette at `g320c+`, scaled by d6 = 0..$10 (2 per step):
```c
void build_fade(void) {           // 9 steps x 32 colours
    for (int step = 0; step <= 0x10; step += 2)
        for (int i = 0; i < 32; i++) {
            uint16_t c = g320c_palette[i];        // $320c(a3) block
            fade[step/2][i] = scale_rgb4(c, step);// per-nibble mulu, >>4
        }
}
```

### $210296 build_copper_planes(d0 = planes-1, a0 = buffer) [verified]
```c
void build_copper_planes(int planes, uint8_t *buf) {
    cop_bplcon0 = (planes << 12) | 0x200;         // -> $7ff22
    uint32_t p = (uint32_t)buf;
    for (int i = 0; i <= planes; i++) {           // copper list at $7ff44
        cop[i].bplpth = p >> 16; cop[i].bplptl = p & 0xffff;
        p += 0x1f40;                              // plane stride
    }
}
```

### $210272 load_palette(void) [verified]
Copy 32 words from `$5400 + g3000*$40` (current fade bank) into the
copper palette entries at $7ff74.  `g3000` = bank selector.

## 3. VBLANK dispatcher ($20ddd4) [verified]
```c
void vblank_tail(void) {          // save-all
    pt_cmd(0, 0);                 // *$7d4c4: PT interface, cmd 0
    void *ctx = a0_result;        // -> $7d4c8
    int idx = g2fc0;              // phase index
    pt_cmd(4, phase_table[idx]);  // table at $20de12, d0=4
    music_tick(ctx);              // *$7d4c8 = $62ca8 ProTracker player
}                                 // rte at $20f7d2 -> parked main loop
```

## 4. ProTracker command interface ($62810) [verified]

Counted PC-relative jump table, table $62826, 9 commands; d0 = command,
d1 = param.  Observed live: 0, 2, 3, 4.  Handler map in
`re/pipeline/handlers.txt`; semantics so far:

| cmd | handler | effect |
|-----|---------|--------|
| 0 | $6283a | register context: store a0 into the list at $62b84 |
| 1 | $62902 | (unverified) calls $62b8a + $62c2a |
| 2 | $6290c | (unverified) calls $62b8a |
| 3 | $62912 | select song: a5 = $63544; a1 = *(song+$10) + $14 |
| 4 | $62986 | write d1 -> song+$20e (tempo/speed word) |
| 5 | $62990 | set flag $62b88 (enable?) |
| 6 | $62998 | clear flag $62b88 (disable?) |
| 7 | $629a0 | indexed read: a2 = $63544 + d1 |
| 8 | $629c0 | indexed read: d1 *= 4 into the same table |

## 5. Gameplay: road renderer (FUN_00213534, $213534-$214726)

Contract-level [contract], inner loop [verified]:

```c
// per segment, inner loop $2142a4-$21433e:
//   A4 -= 8 per iter: 8-byte keyframe records, down from $206184
//   A0 -= 2 per iter: word stream, down from $20cc94
//   d1 = keyframe value; interpolate toward d5 (cmp/sub/bmi $2142c0-);
//   perspective scale via zoom table $204428 ($40(a2,d3.w), 4-byte
//   entries); accumulate into scanline edge buffers $205d88/$206188.
// per band: program the blitter and kick it:
//   BLTAPT = src; BLTDPT = $20ce94[seg] + ((0xb1 - line) >> 3);
//   BLTCON0 = $216fb2[(0xb1 - line) & 0xf]; BLTSIZE = $95..;
//   poll DMACONR bit 6 (BBUSY) before reuse.
```

## Not yet decoded (called by the sequencer, contract unknown)

$20e994, $20eaca, $2105fa, $21064c, $20ff90, $2105e4, $21024a,
$2110f0, $211582, $210a88, $212c52 (RNG-ish: result masked with 7/1),
$212c9e's queue producers.  Decode on demand during porting; the
register trace at `re/pipeline/statelog_gameplay.bin` covers them all.
