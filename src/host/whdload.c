#include "amiga.h"
#include "whdload.h"
#include "cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

long whd_call_count;
long whd_disk_loads;
long whd_disk_bytes;
long whd_file_loads;
long whd_patch_commands;

static bool active;
static WhdConfig config;
static char install_dir[512];
/* Disk images, read once each and kept.  One slot per disk number: a
 * two-disk title (Uridium 2) asks for Disk.1 and Disk.2 by turns, and
 * caching only the first image silently fed it disk 1 for every load. */
#define MAX_DISKS 16
static uint8_t *disk_images[MAX_DISKS];
static long disk_sizes[MAX_DISKS];
static uint8_t *disk_image;          /* the image of the disk last opened */
static long disk_size;
static uint32_t slave_base = WHD_SLAVE_BASE;

/* ---------------------------------------------------------------- memory */

static uint8_t *ram(uint32_t address, uint32_t length)
{
    uint8_t *pointer = amiga_ram(address, length);
    if (!pointer)
        fprintf(stderr, "whdload: $%06x+%u is not RAM\n", address, length);
    return pointer;
}

static uint32_t rd(uint32_t address, int size)
{
    const uint8_t *pointer = ram(address, (uint32_t)size);
    uint32_t value = 0;
    if (!pointer) return 0;
    for (int i = 0; i < size; i++) value = (value << 8) | pointer[i];
    return value;
}

static void wr(uint32_t address, uint32_t value, int size)
{
    uint8_t *pointer = ram(address, (uint32_t)size);
    if (!pointer) return;
    for (int i = size - 1; i >= 0; i--) {
        pointer[i] = (uint8_t)value;
        value >>= 8;
    }
}

static void read_string(uint32_t address, char *out, size_t limit)
{
    size_t i = 0;
    while (i + 1 < limit) {
        const uint8_t *byte = amiga_ram(address + (uint32_t)i, 1);
        if (!byte || !*byte) break;
        out[i++] = (char)*byte;
    }
    out[i] = 0;
}

/* WHDLoad refuses absolute and parent paths; a host that resolves file names
 * against a directory has to do the same or a slave bug becomes a host
 * vulnerability. */
static bool safe_name(const char *name)
{
    if (!*name || *name == '/' || strchr(name, ':')) return false;
    if (strstr(name, "..") || strstr(name, "//")) return false;
    return name[strlen(name) - 1] != '/';
}

static FILE *open_file(const char *name, const char *mode, char *shown,
                       size_t limit)
{
    if (!safe_name(name)) {
        fprintf(stderr, "whdload: refusing file name '%s'\n", name);
        return NULL;
    }
    snprintf(shown, limit, "%s/%s", install_dir, name);
    return fopen(shown, mode);
}

/* ------------------------------------------------------------------ files */

static uint32_t file_size(uint32_t name_address)
{
    char name[256], path[800];
    read_string(name_address, name, sizeof name);
    FILE *file = open_file(name, "rb", path, sizeof path);
    if (!file) return 0;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fclose(file);
    return size > 0 ? (uint32_t)size : 0;
}

static uint32_t load_file(uint32_t name_address, uint32_t destination)
{
    char name[256], path[800];
    read_string(name_address, name, sizeof name);
    FILE *file = open_file(name, "rb", path, sizeof path);
    if (!file) {
        /* Not an error: SWIV asks for its high-score table before it has ever
         * been written, and checks the returned length. */
        fprintf(stderr, "whdload: no file '%s'\n", name);
        return 0;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    uint8_t *pointer = size > 0 ? ram(destination, (uint32_t)size) : NULL;
    size_t got = pointer ? fread(pointer, 1, (size_t)size, file) : 0;
    fclose(file);
    whd_file_loads++;
    fprintf(stderr, "whdload: load '%s' -> $%06x (%zu bytes)\n", name,
            destination, got);
    return (uint32_t)got;
}

static uint32_t save_file(uint32_t name_address, uint32_t source,
                          uint32_t length)
{
    char name[256], path[800];
    read_string(name_address, name, sizeof name);
    const uint8_t *pointer = ram(source, length);
    FILE *file = pointer ? open_file(name, "wb", path, sizeof path) : NULL;
    if (!file) return 0;
    size_t put = fwrite(pointer, 1, length, file);
    bool ok = fclose(file) == 0 && put == length;
    fprintf(stderr, "whdload: save '%s' (%u bytes)%s\n", name, length,
            ok ? "" : " FAILED");
    return ok ? 0xffffffffu : 0;
}

/* ------------------------------------------------------------- disk image */

static bool open_disk(int number)
{
    if (number < 0 || number >= MAX_DISKS) return false;
    if (disk_images[number]) {
        disk_image = disk_images[number];
        disk_size = disk_sizes[number];
        return true;
    }
    char name[64], path[800];
    snprintf(name, sizeof name, "Disk.%d", number);
    FILE *file = open_file(name, "rb", path, sizeof path);
    if (!file) {
        perror(path);
        return false;
    }
    fseek(file, 0, SEEK_END);
    disk_size = ftell(file);
    rewind(file);
    disk_image = malloc((size_t)disk_size);
    if (!disk_image || fread(disk_image, 1, (size_t)disk_size, file)
        != (size_t)disk_size) {
        fprintf(stderr, "whdload: cannot read %s\n", path);
        fclose(file);
        return false;
    }
    fclose(file);
    disk_images[number] = disk_image;
    disk_sizes[number] = disk_size;
    fprintf(stderr, "whdload: %s is %ld bytes\n", name, disk_size);
    return true;
}

/* resload_DiskLoad(d0 = offset, d1 = length, d2 = disk, a0 = destination).
 * The offset is a plain byte offset into the image: WHDLoad's imager stores
 * the decoded sectors back to back, which is exactly how SWIV's own loader
 * addresses the floppy. */
static uint32_t disk_load(uint32_t offset, uint32_t length, uint32_t number,
                          uint32_t destination)
{
    /* d2 is a UWORD: the high half is whatever the caller left there. */
    number &= 0xffff;
    if (!open_disk((int)number)) return 0;
    uint8_t *pointer = ram(destination, length);
    if (!pointer) return 0;
    if (offset >= (uint32_t)disk_size) {
        fprintf(stderr, "whdload: DiskLoad past end of image "
                "($%x of $%lx)\n", offset, disk_size);
        return 0;
    }
    uint32_t available = (uint32_t)disk_size - offset;
    uint32_t copied = length < available ? length : available;
    memcpy(pointer, disk_image + offset, copied);
    if (copied < length) {
        /* The image ends at the last sector the imager found in use, and
         * SWIV's final load asks for a little more than that.  Zero the tail
         * rather than failing the call: the real disk has nothing there
         * either. */
        memset(pointer + copied, 0, length - copied);
        fprintf(stderr, "whdload: DiskLoad tail zero-filled "
                "($%x bytes past $%lx)\n", length - copied, disk_size);
    }
    whd_disk_loads++;
    whd_disk_bytes += length;
    fprintf(stderr, "whdload: frame %ld DiskLoad disk %u $%06x+$%x -> $%06x\n",
            swiv_frame_no, number, offset, length, destination);
    return 0xffffffffu;
}

/* --------------------------------------------------------------- CRC / tags */

/* resload_CRC16 is CRC-16/ARC: reflected, polynomial $A001, initial value
 * zero.  SWIV's slave uses it to tell the supported disk images apart, so a
 * wrong variant shows up immediately as an unknown-version abort. */
static uint16_t crc16(uint32_t address, uint32_t length)
{
    const uint8_t *pointer = ram(address, length);
    uint16_t crc = 0;
    if (!pointer) return 0;
    for (uint32_t i = 0; i < length; i++) {
        crc ^= pointer[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xa001)
                            : (uint16_t)(crc >> 1);
    }
    return crc;
}

#define WHDLTAG(n) (0x88000000u + (n))
enum {
    TAG_ATTNFLAGS = 0, TAG_ECLOCKFREQ = 1, TAG_MONITOR = 2,
    TAG_BUTTONWAIT = 6, TAG_CUSTOM1 = 7, TAG_CBSWITCH = 12,
    TAG_CHIPREVBITS = 13, TAG_IOERR = 14, TAG_CBAF = 16, TAG_VERSION = 17,
    TAG_REVISION = 18, TAG_BUILD = 19, TAG_BPLCON0 = 21,
    TAG_CHKBLTWAIT = 23, TAG_CHKBLTSIZE = 24, TAG_CHKBLTHOG = 25,
    TAG_CHKCOLBST = 26, TAG_LANG = 27, TAG_CHKCOPCON = 30
};

/* resload_Control: a tag list whose ti_Data field is written in place for the
 * _GET tags.  What the host answers here decides which code path the slave
 * takes, so each answer describes this host: a plain 68000 and OCS. */
static uint32_t control(uint32_t list)
{
    for (int guard = 0; guard < 64; guard++, list += 8) {
        uint32_t tag = rd(list, 4);
        if (!tag) return 0xffffffffu;
        uint32_t data = list + 4;
        if ((tag & 0xffffff00u) != 0x88000000u) {
            fprintf(stderr, "whdload: Control: unknown tag $%08x\n", tag);
            continue;
        }
        switch (tag & 0xff) {
        case TAG_ATTNFLAGS:   wr(data, 0, 4); break;   /* 68000, no FPU/MMU */
        case TAG_CHIPREVBITS: wr(data, 0, 4); break;   /* OCS Agnus/Denise */
        case TAG_ECLOCKFREQ:  wr(data, 709379, 4); break;      /* PAL */
        case TAG_MONITOR:     wr(data, 0x00021000, 4); break;  /* PAL id */
        case TAG_BUTTONWAIT:  wr(data, config.buttonwait ? 0xffffffffu : 0, 4);
                              break;
        case TAG_CUSTOM1: case TAG_CUSTOM1 + 1: case TAG_CUSTOM1 + 2:
        case TAG_CUSTOM1 + 3: case TAG_CUSTOM1 + 4:
            wr(data, config.custom[(tag & 0xff) - TAG_CUSTOM1], 4);
            break;
        case TAG_VERSION:     wr(data, 19, 4); break;
        case TAG_REVISION:    wr(data, 2, 4); break;
        case TAG_BUILD:       wr(data, 0, 4); break;
        case TAG_IOERR:       wr(data, 0, 4); break;
        case TAG_BPLCON0:     wr(data, 0x0200, 4); break;
        case TAG_LANG:        wr(data, 0, 4); break;
        /* The _SET tags configure WHDLoad's own sanity checks and callbacks;
         * this host performs neither, so accepting them is correct. */
        case TAG_CBSWITCH: case TAG_CBAF: case TAG_CHKBLTWAIT:
        case TAG_CHKBLTSIZE: case TAG_CHKBLTHOG: case TAG_CHKCOLBST:
        case TAG_CHKCOPCON:
            break;
        default:
            fprintf(stderr, "whdload: Control: unhandled WHDLTAG %u\n",
                    tag & 0xff);
            break;
        }
    }
    fprintf(stderr, "whdload: Control: tag list did not terminate\n");
    return 0;
}

/* ------------------------------------------------------------------ input */

/* resload_ReadJoyPort is why this slave demands WHDLoad 19: SWIV reads its
 * controllers through WHDLoad rather than the hardware, so that a CD32 pad
 * works.  The result is lowlevel.library/ReadJoyPort compatible.
 *
 * This host offers plain two-button joysticks, so the type is always JOYSTK:
 * claiming GAMECTRL would promise pad buttons that nothing here can deliver.
 * Mouse reads are answered as a mouse with no movement and the same buttons,
 * which is what an empty port with a stick in it looks like. */
#define RJP_DETECT      (1u << 31)
#define RJP_WANTMOUSE   (1u << 30)
#define RJP_TYPE_MOUSE  (2u << 28)
#define RJP_TYPE_JOYSTK (3u << 28)
#define RJP_BLUE        (1u << 23)
#define RJP_RED         (1u << 22)
#define RJP_PLAY        (1u << 17)

static uint32_t read_joy_port(uint32_t flags)
{
    unsigned port = flags & 3;
    /* Ports 0 and 1 are the two Amiga ports; 2 and 3 would be a parallel
     * four-player adapter, which this host does not have. */
    uint8_t stick = port < 2 ? joy_state[port] : 0;
    uint32_t state = (flags & RJP_WANTMOUSE) ? RJP_TYPE_MOUSE : RJP_TYPE_JOYSTK;
    if (stick & 0x01) state |= 1u << 3;      /* up */
    if (stick & 0x02) state |= 1u << 2;      /* down */
    if (stick & 0x04) state |= 1u << 1;      /* left */
    if (stick & 0x08) state |= 1u << 0;      /* right */
    if (stick & 0x10) state |= RJP_RED;      /* fire */
    if (stick & 0x20) state |= RJP_BLUE;     /* second button */
    if (stick & 0x40) state |= RJP_PLAY;     /* third button */
    return state;
}

/* ------------------------------------------------------------ patch lists */

enum {
    PL_END, PL_R, PL_P, PL_PS, PL_S, PL_I, PL_B, PL_W, PL_L, PL_A, PL_PA,
    PL_NOP, PL_C, PL_CB, PL_CW, PL_CL, PL_PSS, PL_NEXT, PL_AB, PL_AW, PL_AL,
    PL_DATA, PL_ORB, PL_ORW, PL_ORL, PL_GA, PL_BKPT, PL_BELL,
    PL_IFBW, PL_IFC1, PL_IFC2, PL_IFC3, PL_IFC4, PL_IFC5,
    PL_IFC1X, PL_IFC2X, PL_IFC3X, PL_IFC4X, PL_IFC5X, PL_ELSE, PL_ENDIF,
    PL_VB, PL_VW, PL_VL, PL_ANDB, PL_ANDW, PL_ANDL,
    PL_IFC1EQ, PL_IFC2EQ, PL_IFC3EQ, PL_IFC4EQ, PL_IFC5EQ,
    PL_IFC1RG, PL_IFC2RG, PL_IFC3RG, PL_IFC4RG, PL_IFC5RG,
    PL_IFVB, PL_IFVW, PL_IFVL, PL_IFVBEQ, PL_IFVWEQ, PL_IFVLEQ
};
#define PLCMDF_WORDADR 0x8000
#define PLCMDF_CTRL    0x4000

static void patch_bra(uint32_t where, int32_t distance)
{
    /* PL_S carries the displacement a BRA needs, already biased by -2.  Use
     * the short form when it fits, which overwrites two bytes instead of
     * four: a patch that clobbers more of the original than it must is how a
     * skip turns into a crash further down. */
    if (distance >= -128 && distance <= 127 && distance != 0)
        wr(where, 0x6000u | (uint32_t)(distance & 0xff), 2);
    else {
        wr(where, 0x6000, 2);
        wr(where + 2, (uint32_t)distance & 0xffff, 2);
    }
}

static void apply_patch_list(uint32_t list, uint32_t base)
{
    uint32_t at = list;
    /* Conditional blocks nest.  Every command is decoded even while skipping,
     * because the list is a byte stream and skipping is not the same as
     * stopping. */
    struct { bool executing, taken; } block[16];
    int depth = 0;
    bool executing = true;

    for (int guard = 0; guard < 8192; guard++) {
        uint32_t command = rd(at, 2);
        at += 2;
        uint32_t number = command & 0x3fff;
        uint32_t address = 0;
        if (!(command & PLCMDF_CTRL)) {
            if (command & PLCMDF_WORDADR) {
                address = rd(at, 2);
                at += 2;
            } else {
                address = rd(at, 4);
                at += 4;
            }
        }
        uint32_t where = base + address;
        whd_patch_commands++;

        /* Arguments first, so `at` always advances by the full command even
         * when the command is inside a skipped block. */
        uint32_t argument = 0, argument2 = 0;
        switch (number) {
        case PL_P: case PL_PS: case PL_S: case PL_B: case PL_W: case PL_PA:
        case PL_NOP: case PL_C: case PL_NEXT: case PL_AB: case PL_AW:
        case PL_DATA: case PL_ORB: case PL_ORW: case PL_GA: case PL_BELL:
        case PL_VB: case PL_VW: case PL_VL: case PL_ANDB: case PL_ANDW:
        case PL_IFC1X: case PL_IFC2X: case PL_IFC3X: case PL_IFC4X:
        case PL_IFC5X: case PL_IFC1EQ: case PL_IFC2EQ: case PL_IFC3EQ:
        case PL_IFC4EQ: case PL_IFC5EQ: case PL_IFVB: case PL_IFVW:
        case PL_IFVL:
            argument = rd(at, 2);
            at += 2;
            break;
        case PL_L: case PL_A: case PL_AL: case PL_ORL: case PL_ANDL:
            argument = rd(at, 4);
            at += 4;
            break;
        case PL_PSS: case PL_IFC1RG: case PL_IFC2RG: case PL_IFC3RG:
        case PL_IFC4RG: case PL_IFC5RG: case PL_IFVBEQ: case PL_IFVWEQ:
            argument = rd(at, 2);
            argument2 = rd(at + 2, 2);
            at += 4;
            break;
        case PL_IFVLEQ:                 /* word source, then a LONG value */
            argument = rd(at, 2);
            argument2 = rd(at + 2, 4);
            at += 6;
            break;
        default:
            break;
        }
        /* Destinations and sources given as "inside the slave" are offsets
         * from the start of the list itself -- and SIGNED: the macros emit
         * "label - .patchlist", and slaves routinely share routines that sit
         * BEFORE the list, or chain to a list further back.  Reading these as
         * unsigned put every jump tens of kilobytes past the slave and the
         * game ran off into unmapped memory a few frames later. */
        uint32_t inside = list + (uint32_t)(int32_t)(int16_t)argument;

        if (number >= PL_IFBW && number <= PL_ENDIF && number != PL_ELSE &&
            number != PL_ENDIF) {
            bool condition = false;
            switch (number) {
            case PL_IFBW: condition = config.buttonwait; break;
            case PL_IFC1: case PL_IFC2: case PL_IFC3: case PL_IFC4:
            case PL_IFC5:
                condition = config.custom[number - PL_IFC1] != 0;
                break;
            case PL_IFC1X: case PL_IFC2X: case PL_IFC3X: case PL_IFC4X:
            case PL_IFC5X:
                condition = (config.custom[number - PL_IFC1X] >>
                             (argument & 31)) & 1;
                break;
            default: break;
            }
            if (depth == (int)(sizeof block / sizeof block[0])) {
                fprintf(stderr, "whdload: patch list nested too deeply\n");
                return;
            }
            block[depth].executing = executing;
            block[depth].taken = condition;
            depth++;
            executing = executing && condition;
            continue;
        }
        if (number >= PL_IFC1EQ && number <= PL_IFVLEQ) {
            bool condition = false;
            if (number >= PL_IFC1EQ && number <= PL_IFC5EQ)
                condition = (config.custom[number - PL_IFC1EQ] & 0xffff) ==
                            argument;
            else if (number >= PL_IFC1RG && number <= PL_IFC5RG) {
                uint32_t value = config.custom[number - PL_IFC1RG] & 0xffff;
                condition = value >= argument && value <= argument2;
            } else if (number == PL_IFVB)
                condition = rd(inside, 1) != 0;
            else if (number == PL_IFVW)
                condition = rd(inside, 2) != 0;
            else if (number == PL_IFVL)
                condition = rd(inside, 4) != 0;
            else if (number == PL_IFVBEQ)
                condition = rd(list + argument, 1) == (argument2 & 0xff);
            else if (number == PL_IFVWEQ)
                condition = rd(list + argument, 2) == argument2;
            else if (number == PL_IFVLEQ)
                condition = rd(list + argument, 4) == argument2;
            if (depth == (int)(sizeof block / sizeof block[0])) {
                fprintf(stderr, "whdload: patch list nested too deeply\n");
                return;
            }
            block[depth].executing = executing;
            block[depth].taken = condition;
            depth++;
            executing = executing && condition;
            continue;
        }
        if (number == PL_ELSE) {
            if (!depth) {
                fprintf(stderr, "whdload: PL_ELSE outside a block\n");
                return;
            }
            executing = block[depth - 1].executing && !block[depth - 1].taken;
            continue;
        }
        if (number == PL_ENDIF) {
            if (!depth) {
                fprintf(stderr, "whdload: PL_ENDIF outside a block\n");
                return;
            }
            executing = block[--depth].executing;
            continue;
        }

        if (number == PL_DATA) {
            uint32_t length = argument;
            if (executing) {
                const uint8_t *source = ram(at, length);
                uint8_t *target = ram(where, length);
                if (source && target) memmove(target, source, length);
            }
            at += length + (length & 1);       /* the list stays word aligned */
            continue;
        }
        if (number == PL_END) return;
        if (number == PL_NEXT) {
            if (executing) {
                if (depth) fprintf(stderr, "whdload: PL_NEXT inside a block\n");
                apply_patch_list(inside, base);
            }
            return;
        }

        if (!executing) continue;

        switch (number) {
        case PL_R:   wr(where, 0x4e75, 2); break;               /* rts */
        case PL_I:   wr(where, 0x4afc, 2); break;               /* illegal */
        case PL_BKPT: wr(where, 0x4afc, 2); break;   /* no freezer to enter */
        case PL_BELL: break;                         /* nothing to flash */
        case PL_P:   wr(where, 0x4ef9, 2);           /* jmp  inside slave */
                     wr(where + 2, inside, 4); break;
        case PL_PS:  wr(where, 0x4eb9, 2);           /* jsr  inside slave */
                     wr(where + 2, inside, 4); break;
        case PL_PSS: wr(where, 0x4eb9, 2);
                     wr(where + 2, inside, 4);
                     for (uint32_t i = 0; i < argument2; i += 2)
                         wr(where + 6 + i, 0x4e71, 2);
                     break;
        case PL_S:   patch_bra(where, (int16_t)argument); break;
        case PL_B:   wr(where, argument & 0xff, 1); break;
        case PL_W:   wr(where, argument, 2); break;
        case PL_L:   wr(where, argument, 4); break;
        case PL_A:   wr(where, base + argument, 4); break;
        case PL_PA:  wr(where, inside, 4); break;
        case PL_GA:  wr(inside, where, 4); break;
        case PL_NOP: for (uint32_t i = 0; i < argument; i += 2)
                         wr(where + i, 0x4e71, 2);
                     break;
        case PL_C: {
            uint32_t length = argument ? argument : 0x10000;
            uint8_t *target = ram(where, length);
            if (target) memset(target, 0, length);
            break;
        }
        case PL_CB:  wr(where, 0, 1); break;
        case PL_CW:  wr(where, 0, 2); break;
        case PL_CL:  wr(where, 0, 4); break;
        case PL_AB:  wr(where, rd(where, 1) + argument, 1); break;
        case PL_AW:  wr(where, rd(where, 2) + argument, 2); break;
        case PL_AL:  wr(where, rd(where, 4) + argument, 4); break;
        case PL_ORB: wr(where, rd(where, 1) | argument, 1); break;
        case PL_ORW: wr(where, rd(where, 2) | argument, 2); break;
        case PL_ORL: wr(where, rd(where, 4) | argument, 4); break;
        case PL_ANDB: wr(where, rd(where, 1) & argument, 1); break;
        case PL_ANDW: wr(where, rd(where, 2) & argument, 2); break;
        case PL_ANDL: wr(where, rd(where, 4) & argument, 4); break;
        case PL_VB:  wr(where, rd(inside, 1), 1); break;
        case PL_VW:  wr(where, rd(inside, 2), 2); break;
        case PL_VL:  wr(where, rd(inside, 4), 4); break;
        default:
            /* Fail loudly: a command decoded as something it is not would
             * write the wrong bytes into the game and be near impossible to
             * find later. */
            fprintf(stderr, "whdload: unknown patch command $%04x at $%06x\n",
                    command, at);
            amiga_stop();
            return;
        }
    }
    fprintf(stderr, "whdload: patch list at $%06x did not end\n", list);
    amiga_stop();
}

/* ------------------------------------------------------------------ traps */

static const char *reason_text(uint32_t reason)
{
    switch (reason) {
    case 0:  return "OK";
    case 1:  return "DEBUG";
    case 30: return "WRONGVER";
    case 31: return "DISKLOAD";
    case 43: return "FAILMSG";
    default: return "see TDREASON_#? in whdload.i";
    }
}

bool whdload_trap(uint32_t pc)
{
    if (!active) return false;
    if (pc < WHD_RESLOAD_BASE || pc >= WHD_RESLOAD_BASE + WHD_RESLOAD_SIZE)
        return false;

    uint32_t offset = pc - WHD_RESLOAD_BASE;
    uint32_t d0 = cpu_get_reg(CPU_REG_D0);
    uint32_t d1 = cpu_get_reg(CPU_REG_D1);
    uint32_t d2 = cpu_get_reg(CPU_REG_D2);
    uint32_t a0 = cpu_get_reg(CPU_REG_A0);
    uint32_t a1 = cpu_get_reg(CPU_REG_A1);
    uint32_t stack = cpu_get_reg(CPU_REG_A7);
    uint32_t result = 0;
    whd_call_count++;

    switch (offset) {
    case WHD_ABORT: {
        /* The one function called by JMP, with its arguments on the stack. */
        uint32_t reason = rd(stack, 4);
        fprintf(stderr, "whdload: slave aborted, reason %u (%s), "
                "primary $%x secondary $%x\n", reason, reason_text(reason),
                rd(stack + 4, 4), rd(stack + 8, 4));
        amiga_pc_history();
        amiga_stop();
        /* Abort never returns.  Park the CPU on the abort entry so the rest
         * of the frame does not walk into the table behind it. */
        cpu_set_reg(CPU_REG_PC, WHD_RESLOAD_BASE + WHD_ABORT);
        cpu_end_timeslice();
        return true;
    }
    case WHD_LOADFILE:
    case WHD_LOADFILEDECRUNCH:
        /* Nothing SWIV loads as a file is crunched; the only one is its own
         * high-score table. */
        result = load_file(a0, a1);
        break;
    case WHD_DECRUNCH: {
        /* Lotus 2's slave ships its own install body as an RNC2-compressed
         * blob and uses resload_Decrunch to inflate it.  The host's
         * decompressor is rnc2_unpack(); see rnc2.c for the algorithm. */
        extern int rnc2_unpack(const uint8_t *input, uint8_t *output,
                               uint32_t *out_size);
        if (a0 == 0 || a1 == 0) {
            fprintf(stderr, "whdload: Decrunch: bad args a0=$%06x a1=$%06x\n",
                    a0, a1);
            amiga_pc_history();
            amiga_stop();
            cpu_end_timeslice();
            return true;
        }
        uint32_t out_size = 0;
        const uint8_t *src = amiga_ram(a0, 18);
        if (!src) {
            fprintf(stderr, "whdload: Decrunch: source $%06x not in RAM\n", a0);
            amiga_pc_history();
            amiga_stop();
            cpu_end_timeslice();
            return true;
        }
        uint32_t unpacked = ((uint32_t)src[4] << 24) |
                            ((uint32_t)src[5] << 16) |
                            ((uint32_t)src[6] << 8) | src[7];
        uint32_t packed = ((uint32_t)src[8] << 24) |
                          ((uint32_t)src[9] << 16) |
                          ((uint32_t)src[10] << 8) | src[11];
        uint8_t *dst = amiga_ram(a1, unpacked ? unpacked : 1);
        if (!dst) {
            fprintf(stderr, "whdload: Decrunch: dest $%06x + %u not in RAM\n",
                    a1, unpacked);
            amiga_pc_history();
            amiga_stop();
            cpu_end_timeslice();
            return true;
        }
        const uint8_t *full_src = amiga_ram(a0, 18 + packed);
        if (!full_src) {
            fprintf(stderr, "whdload: Decrunch: packed body $%06x + %u not "
                    "in RAM\n", a0, packed);
            amiga_pc_history();
            amiga_stop();
            cpu_end_timeslice();
            return true;
        }
        int rc = rnc2_unpack(full_src, dst, &out_size);
        fprintf(stderr, "whdload: Decrunch $%06x -> $%06x packed=%u unpacked=%u/%u rc=%d\n",
                a0, a1, packed, out_size, unpacked, rc);
        if (rc == 0) {
            result = unpacked;
        } else {
            /* Stop the slave: a partial or wrong decompressed install body
             * is worse than no body at all, because the slave will execute
             * the corrupted bytes as code. */
            fprintf(stderr, "whdload: Decrunch FAILED (%d) -- stopping slave\n",
                    rc);
            amiga_pc_history();
            amiga_stop();
            cpu_end_timeslice();
            return true;
        }
        break;
    }
    case WHD_GETFILESIZE:
    case WHD_GETFILESIZEDEC:
        result = file_size(a0);
        break;
    case WHD_SAVEFILE:
        result = save_file(a0, a1, d0);
        break;
    case WHD_DISKLOAD:
        result = disk_load(d0, d1, d2, a0);
        break;
    case WHD_CRC16:
        result = crc16(a0, d0);
        fprintf(stderr, "whdload: CRC16 $%06x+$%x -> $%04x\n", a0, d0, result);
        break;
    case WHD_CONTROL:
        result = control(a0);
        break;
    case WHD_PATCH:
        fprintf(stderr, "whdload: Patch list $%06x -> $%06x\n", a0, a1);
        apply_patch_list(a0, a1);
        break;
    case WHD_READJOYPORT:
        result = read_joy_port(d0);
        break;
    case WHD_SETCPU:
        /* d0 = requested properties, d1 = mask; a 68000 has none of them. */
        result = 0;
        break;
    case WHD_DELAY:
        /* d0 = tenths of a second.  Nothing here is real time, and the frame
         * loop is the clock, so the wait is skipped deliberately. */
        break;
    case WHD_SETCACR:
    case WHD_FLUSHCACHE:
    case WHD_PROTECTREAD:
    case WHD_PROTECTREADWRITE:
    case WHD_PROTECTWRITE:
    case WHD_PROTECTREMOVE:
    case WHD_PROTECTSMC:
        break;                          /* no caches and no MMU to program */
    default:
        fprintf(stderr, "whdload: UNIMPLEMENTED resload offset $%02x "
                "(d0=$%x d1=$%x a0=$%06x a1=$%06x)\n", offset, d0, d1, a0, a1);
        amiga_pc_history();
        amiga_stop();
        cpu_end_timeslice();
        return true;
    }

    /* Return to the caller: pop the return address the JSR pushed. */
    cpu_set_reg(CPU_REG_A7, stack + 4);
    cpu_set_reg(CPU_REG_PC, rd(stack, 4));
    cpu_set_reg(CPU_REG_D0, result);
    return true;
}

/* ------------------------------------------------------------------- boot */

/* The slave is an ordinary AmigaDOS executable.  Load its single code hunk
 * and apply any relocations, so it works wherever this host parks it. */
static bool load_slave(const char *path, uint32_t base, uint32_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return false;
    }
    long length;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    rewind(file);
    uint8_t *image = malloc((size_t)length);
    if (!image || fread(image, 1, (size_t)length, file) != (size_t)length) {
        fprintf(stderr, "%s: short read\n", path);
        fclose(file);
        free(image);
        return false;
    }
    fclose(file);

    bool ok = false;
    uint32_t at = 0;
    #define WORD32(offset) ((uint32_t)image[offset] << 24 | \
                            (uint32_t)image[(offset) + 1] << 16 | \
                            (uint32_t)image[(offset) + 2] << 8 | \
                            image[(offset) + 3])
    if (length < 0x20 || WORD32(0) != 0x3f3) {
        fprintf(stderr, "%s: not an Amiga executable\n", path);
        goto done;
    }
    if (WORD32(8) != 1) {
        fprintf(stderr, "%s: %u hunks, expected one\n", path, WORD32(8));
        goto done;
    }
    at = 0x18;
    if (WORD32(at) != 0x3e9) {
        fprintf(stderr, "%s: first hunk is not code\n", path);
        goto done;
    }
    *size = WORD32(at + 4) * 4;
    at += 8;
    if ((long)(at + *size) > length) {
        fprintf(stderr, "%s: truncated code hunk\n", path);
        goto done;
    }
    uint8_t *target = amiga_ram(base, *size);
    if (!target) goto done;
    memcpy(target, image + at, *size);
    at += *size;
    while (at + 4 <= (uint32_t)length) {
        uint32_t block = WORD32(at);
        at += 4;
        if (block == 0x3f2) break;                       /* HUNK_END */
        if (block != 0x3ec) {                            /* HUNK_RELOC32 */
            fprintf(stderr, "%s: unexpected hunk block $%x\n", path, block);
            goto done;
        }
        for (;;) {
            uint32_t count = WORD32(at);
            at += 4;
            if (!count) break;
            at += 4;                                     /* target hunk */
            for (uint32_t i = 0; i < count; i++, at += 4) {
                uint32_t place = WORD32(at);
                uint32_t value = (uint32_t)target[place] << 24 |
                                 (uint32_t)target[place + 1] << 16 |
                                 (uint32_t)target[place + 2] << 8 |
                                 target[place + 3];
                value += base;
                target[place] = (uint8_t)(value >> 24);
                target[place + 1] = (uint8_t)(value >> 16);
                target[place + 2] = (uint8_t)(value >> 8);
                target[place + 3] = (uint8_t)value;
            }
        }
    }
    #undef WORD32
    ok = true;
done:
    free(image);
    return ok;
}

bool whdload_boot(const WhdConfig *requested)
{
    config = *requested;
    snprintf(install_dir, sizeof install_dir, "%s", config.dir);
    for (int i = 0; i < MAX_DISKS; i++) {
        free(disk_images[i]);
        disk_images[i] = NULL;
    }
    disk_image = NULL;
    whd_call_count = whd_disk_loads = whd_disk_bytes = 0;
    whd_file_loads = whd_patch_commands = 0;

    char path[800];
    snprintf(path, sizeof path, "%s/%s", install_dir, config.slave);
    uint32_t size = 0;
    if (!load_slave(path, slave_base, &size)) return false;

    if (rd(slave_base, 2) != 0x70ff ||
        memcmp(amiga_ram(slave_base + 4, 8), "WHDLOADS", 8)) {
        fprintf(stderr, "whdload: %s is not a slave\n", path);
        return false;
    }
    uint32_t version = rd(slave_base + 0x0c, 2);
    uint32_t flags = rd(slave_base + 0x0e, 2);
    uint32_t basemem = rd(slave_base + 0x10, 4);
    uint32_t entry = slave_base + rd(slave_base + 0x18, 2);
    uint32_t expmem = rd(slave_base + 0x20, 4);
    /* ws_CurrentDir (slave+$1a): data files live in this sub-directory of the install */
    uint32_t curdir_off = version >= 8 ? rd(slave_base + 0x1a, 2) : 0;
    if (curdir_off) {
        const char *cd = (const char *)amiga_ram(slave_base + curdir_off, 64);
        if (cd && cd[0]) {
            char tmp[512]; snprintf(tmp, sizeof tmp, "%s/%s", install_dir, cd);
            snprintf(install_dir, sizeof install_dir, "%s", tmp);
            fprintf(stderr, "whdload: current directory '%s' -> %s\n", cd, install_dir);
        }
    }
    fprintf(stderr, "whdload: %s v%u flags $%04x basemem $%x expmem $%x "
            "entry $%06x\n", config.slave, version, flags, basemem, expmem,
            entry);
    if (basemem > CHIP_SIZE) {
        fprintf(stderr, "whdload: slave wants $%x of chip RAM, host has $%x\n",
                basemem, (unsigned)CHIP_SIZE);
        return false;
    }
    if (!amiga_ram(WHD_EXPMEM_BASE, expmem)) {
        fprintf(stderr, "whdload: no room for $%x bytes of ExpMem\n", expmem);
        return false;
    }
    /* WHDLoad replaces the requested SIZE with the allocated ADDRESS. */
    wr(slave_base + 0x20, WHD_EXPMEM_BASE, 4);
    /* From version 17 WHDLoad always overwrites ws_keydebug with its own
     * default ($78) or the DebugKey option.  SWIV's slave compares every
     * rawkey against that byte and makes a core dump on a match, so leaving
     * the file's own zero there means rawkey 0 quits the game. */
    wr(slave_base + 0x1e, 0x78, 1);

    /* The resload table is pure marker: every entry is an address the
     * instruction hook recognises.  Fill it with NOPs so a fall-through is
     * harmless and obvious rather than executing whatever was there. */
    for (uint32_t i = 0; i < WHD_RESLOAD_SIZE; i += 2)
        wr(WHD_RESLOAD_BASE + i, 0x4e71, 2);

    /* execbase is deliberately invalid: WHDLoad sets it so that any code
     * still trying to call the operating system faults instead of running
     * with a plausible-looking pointer. */
    wr(4, 0xf0000001u, 4);

    cpu_set_reg(CPU_REG_A0, WHD_RESLOAD_BASE);
    cpu_set_reg(CPU_REG_A7, WHD_STACK_TOP);
    cpu_set_reg(CPU_REG_SR, 0x2000);      /* supervisor, interrupts open */
    cpu_set_reg(CPU_REG_PC, entry);
    active = true;
    return true;
}

bool whdload_active(void) { return active; }

void whdload_report(void)
{
    fprintf(stderr, "whdload: %ld resload calls, %ld disk loads "
            "(%ld bytes), %ld file loads, %ld patch commands\n",
            whd_call_count, whd_disk_loads, whd_disk_bytes, whd_file_loads,
            whd_patch_commands);
}
