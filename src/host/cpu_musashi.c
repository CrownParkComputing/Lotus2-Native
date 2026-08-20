/* cpu_musashi.c -- cpu.h backed by Musashi.  ORACLE BUILD ONLY.
 *
 * This is the reference implementation: it produces the snapshot pairs
 * and frames the native build is measured against.  It is deliberately
 * not part of the native binary.
 */
#include "cpu.h"
#include "m68k.h"

static int reg_of(int r)
{
    if (r >= CPU_REG_D0 && r <= CPU_REG_D7) return M68K_REG_D0 + (r - CPU_REG_D0);
    if (r >= CPU_REG_A0 && r <= CPU_REG_A7) return M68K_REG_A0 + (r - CPU_REG_A0);
    if (r == CPU_REG_PC) return M68K_REG_PC;
    if (r == CPU_REG_SR) return M68K_REG_SR;
    return M68K_REG_PPC;
}

void cpu_init(void) { m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); }
void cpu_reset(void) { m68k_pulse_reset(); }
void cpu_set_irq(unsigned int level) { m68k_set_irq(level); }
unsigned int cpu_get_reg(int r) { return m68k_get_reg(0, reg_of(r)); }
void cpu_set_reg(int r, unsigned int v) { m68k_set_reg(reg_of(r), v); }
int cpu_execute(int cycles) { return m68k_execute(cycles); }
unsigned int cpu_disassemble(char *out, unsigned int pc)
{ return m68k_disassemble(out, pc, M68K_CPU_TYPE_68000); }
void cpu_end_timeslice(void) { m68k_end_timeslice(); }
