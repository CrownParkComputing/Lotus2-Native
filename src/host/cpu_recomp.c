/* cpu_recomp.c -- cpu.h backed by statically recompiled C.  NO EMULATOR.
 *
 * Nothing from Musashi is linked here.  The generated switch in
 * src/recomp/lotus2_recomp.c executes the game's own instructions as
 * compiled host code, and memory goes through the host's existing
 * m68k_read_memory_* / m68k_write_memory_*, so custom-register writes
 * keep every side effect: blitter starts, copper fetches, DMA, Paula.
 * A recompiled CPU writing into a private array would run the game
 * perfectly and display nothing.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cpu.h"
#include "amiga.h"
#include "m68krt.h"

void lotus2_recomp_step(M68K *m);
void swiv_instr_hook(unsigned int pc);

static M68K cpu;
static int pending_irq;
static long steps_total;

/* Real cycles, measured from the oracle.
 *
 * Each generated instruction adds its own cost to m->cycles, so the CPU
 * can be run against the same per-scanline cycle budget the chipset uses
 * -- which is what keeps the beam-position reads, blitter waits and
 * vblank handshakes landing where the game expects them.  The costs come
 * from the oracle's own counter (SWIV_CYCLES), not from a timing manual:
 * the reference frames were produced with those counts, so they are the
 * right definition of correct here.
 */

/* the generated code reaches memory through these */
uint32_t m68krt_read(uint32_t addr, int size)
{
    if (size == 1) return m68k_read_memory_8(addr);
    if (size == 2) return m68k_read_memory_16(addr);
    return m68k_read_memory_32(addr);
}
void m68krt_write(uint32_t addr, int size, uint32_t value)
{
    if (size == 1)      m68k_write_memory_8(addr, value);
    else if (size == 2) m68k_write_memory_16(addr, value);
    else                m68k_write_memory_32(addr, value);
}

void cpu_init(void)
{
    memset(&cpu, 0, sizeof cpu);

}

void cpu_reset(void)
{
    cpu.a[7] = m68krt_read(0, 4);
    cpu.pc   = m68krt_read(4, 4);
    cpu.sr_hi = 0x2700;
    cpu.halted = 0;
    cpu.fault = NULL;
}

void cpu_set_irq(unsigned int level) { pending_irq = (int)level; }

unsigned int cpu_get_reg(int r)
{
    if (r >= CPU_REG_D0 && r <= CPU_REG_D7) return cpu.d[r - CPU_REG_D0];
    if (r >= CPU_REG_A0 && r <= CPU_REG_A7) return cpu.a[r - CPU_REG_A0];
    if (r == CPU_REG_PC) return cpu.pc;
    if (r == CPU_REG_SR) return m68k_get_sr(&cpu);
    return cpu.pc;
}

void cpu_set_reg(int r, unsigned int v)
{
    if (r >= CPU_REG_D0 && r <= CPU_REG_D7) cpu.d[r - CPU_REG_D0] = v;
    else if (r >= CPU_REG_A0 && r <= CPU_REG_A7) cpu.a[r - CPU_REG_A0] = v;
    else if (r == CPU_REG_PC) cpu.pc = v;
    else if (r == CPU_REG_SR) m68k_set_sr(&cpu, (unsigned short)v);
}

/* An instruction budget stands in for a cycle model.
 *
 * The generated code carries no cycle counts.  Rather than invent them,
 * the budget is CALIBRATED: LOTUS2_IPL is chosen so the native build
 * executes the same instructions per frame as the oracle, and the frame
 * gate decides whether that is close enough.  Busy-wait loops (blitter
 * done, VPOS) self-correct -- they consume budget until the chipset
 * moves on.
 */
static int timeslice_over;
void cpu_end_timeslice(void) { timeslice_over = 1; }

long cpu_recomp_steps(void) { return steps_total; }
int cpu_cycles_run(void) { return (int)(cpu.cycles - cpu.cycles_base); }

int cpu_execute(int cycles)
{
    timeslice_over = 0;
    unsigned long budget = cpu.cycles + (unsigned long)cycles;
    cpu.cycles_base = cpu.cycles;
    while (cpu.cycles < budget && !cpu.halted && !timeslice_over) {
        /* Interrupts are taken at ANY instruction boundary, not once per
         * scanline.  The music replay runs from the vblank handler, so
         * deferring an interrupt to the start of the next line shifts its
         * state and eventually sends it down a different branch. */
        if (pending_irq && m68k_take_irq(&cpu, pending_irq))
            pending_irq = 0;
        swiv_instr_hook(cpu.pc);
        lotus2_recomp_step(&cpu);
        steps_total++;
    }
    if (cpu.halted) {
        fprintf(stderr, "cpu_recomp: HALTED pc=$%06x: %s\n",
                cpu.pc, cpu.fault ? cpu.fault : "?");
        amiga_stop();
        cpu.halted = 0;
    }
    return cycles;
}

unsigned int cpu_disassemble(char *out, unsigned int pc)
{ sprintf(out, "$%06x (recompiled)", pc); return 2; }
