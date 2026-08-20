#ifndef SWIV_WHDLOAD_H
#define SWIV_WHDLOAD_H

#include <stdbool.h>
#include <stdint.h>

/* Being WHDLoad for the duration of the run.
 *
 * SWIV has no self-contained loader that a host could simply read in: the
 * entry point of the install is the WHDLoad SLAVE, which is called with A0
 * pointing at the resload jump table and then pulls the game out of the disk
 * image itself, patches it, and jumps into it.  The patches it applies rewire
 * the game's own floppy loader into slave routines that call back into
 * resload, so resload has to keep working for as long as the game runs.
 *
 * So this file implements the WHDLoad side of that contract: place the slave
 * where the game cannot reach it, publish a resload table whose entries are
 * addresses the instruction hook recognises, and service the calls.  No 68000
 * stub code is needed anywhere.
 *
 * The resload jump-table offsets and the patch-list command encoding are taken
 * from WHDLoad's own include file (whdload.i, version 20), not guessed.
 */

/* Slave, resload table and slave stack live above the expansion RAM the slave
 * is given, so nothing the game does can touch them. */
#define WHD_SLAVE_BASE   0x00380000u
#define WHD_RESLOAD_BASE 0x0037f000u
#define WHD_RESLOAD_SIZE 0x00000100u
#define WHD_STACK_TOP    0x0037e000u
/* WHDLoad allocates ws_ExpMem bytes and writes the ADDRESS back into the
 * slave header before entering it. */
#define WHD_EXPMEM_BASE  0x00200000u

/* resload jump-table offsets, in the declaration order of whdload.i. */
#define WHD_INSTALL            0x00
#define WHD_ABORT              0x04
#define WHD_LOADFILE           0x08
#define WHD_SAVEFILE           0x0c
#define WHD_SETCACR            0x10
#define WHD_LISTFILES          0x14
#define WHD_DECRUNCH           0x18
#define WHD_LOADFILEDECRUNCH   0x1c
#define WHD_FLUSHCACHE         0x20
#define WHD_GETFILESIZE        0x24
#define WHD_DISKLOAD           0x28
#define WHD_DISKLOADDEV        0x2c
#define WHD_CRC16              0x30
#define WHD_CONTROL            0x34
#define WHD_SAVEFILEOFFSET     0x38
#define WHD_PROTECTREAD        0x3c
#define WHD_PROTECTREADWRITE   0x40
#define WHD_PROTECTWRITE       0x44
#define WHD_PROTECTREMOVE      0x48
#define WHD_LOADFILEOFFSET     0x4c
#define WHD_RELOCATE           0x50
#define WHD_DELAY              0x54
#define WHD_DELETEFILE         0x58
#define WHD_PROTECTSMC         0x5c
#define WHD_SETCPU             0x60
#define WHD_PATCH              0x64
#define WHD_LOADKICK           0x68
#define WHD_DELTA              0x6c
#define WHD_GETFILESIZEDEC     0x70
#define WHD_PATCHSEG           0x74
#define WHD_EXAMINE            0x78
#define WHD_EXNEXT             0x7c
#define WHD_GETCUSTOM          0x80
#define WHD_VSNPRINTF          0x84
#define WHD_LOG                0x88
#define WHD_READJOYPORT        0x8c

typedef struct {
    const char *dir;        /* the install drawer: slave, Disk.1, saves */
    const char *slave;      /* slave file name inside that drawer */
    /* WHDLoad's Custom1..5 options.  SWIV's slave publishes four of them as
     * C1..C4 in ws_config: jump control, unlimited lives, weapon power and
     * weapon speed, and reads them through resload_Control. */
    uint32_t custom[5];
    bool buttonwait;
} WhdConfig;

/* Load and relocate the slave, publish the resload table, then set the CPU up
 * to enter ws_GameLoader exactly as WHDLoad does. */
bool whdload_boot(const WhdConfig *config);

/* Called for every executed PC.  Returns true when the PC landed inside the
 * resload table, in which case the call has been serviced and the CPU has been
 * returned to the caller. */
bool whdload_trap(uint32_t pc);

bool whdload_active(void);
void whdload_report(void);

/* Diagnostics. */
extern long whd_call_count;      /* resload calls serviced */
extern long whd_disk_loads;      /* resload_DiskLoad calls */
extern long whd_disk_bytes;
extern long whd_file_loads;      /* resload_LoadFile/Decrunch calls */
extern long whd_patch_commands;  /* patch-list commands applied */

#endif
