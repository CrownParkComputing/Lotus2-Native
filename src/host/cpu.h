/* cpu.h -- the CPU interface the host uses.
 *
 * The host's chipset is title-neutral and correct; only the CPU behind it
 * changes.  Two implementations satisfy this interface:
 *
 *   src/host/cpu_recomp.c   generated C  (the native build -- no emulator)
 *   src/host/cpu_musashi.c  Musashi      (the ORACLE build only)
 *
 * Musashi is a development tool from here on: it produces the snapshot
 * pairs and reference frames the native build is judged against, and it
 * is not linked into the native binary at all.
 */
#ifndef LOTUS2_CPU_H
#define LOTUS2_CPU_H

enum {
    CPU_REG_D0 = 0, CPU_REG_D1, CPU_REG_D2, CPU_REG_D3,
    CPU_REG_D4, CPU_REG_D5, CPU_REG_D6, CPU_REG_D7,
    CPU_REG_A0, CPU_REG_A1, CPU_REG_A2, CPU_REG_A3,
    CPU_REG_A4, CPU_REG_A5, CPU_REG_A6, CPU_REG_A7,
    CPU_REG_PC, CPU_REG_SR, CPU_REG_PPC
};

void cpu_init(void);
void cpu_reset(void);
void cpu_set_irq(unsigned int level);
unsigned int cpu_get_reg(int regnum);
void cpu_set_reg(int regnum, unsigned int value);
int  cpu_execute(int cycles);
unsigned int cpu_disassemble(char *out, unsigned int pc);
/* cut the current timeslice short (the slave uses it to yield) */
void cpu_end_timeslice(void);
/* cycles consumed so far in the current cpu_execute() call */
int cpu_cycles_run(void);

/* the host's memory layer, shared by both implementations */
unsigned int m68k_read_memory_8(unsigned int address);
unsigned int m68k_read_memory_16(unsigned int address);
unsigned int m68k_read_memory_32(unsigned int address);
void m68k_write_memory_8(unsigned int address, unsigned int value);
void m68k_write_memory_16(unsigned int address, unsigned int value);
void m68k_write_memory_32(unsigned int address, unsigned int value);

#endif
