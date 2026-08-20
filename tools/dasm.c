/* dasm.c -- disassemble a range of the combined image (chip $0, fast
 * $200000, slave $380000) with Musashi's m68kdasm.
 *
 *   cc -O2 -Ithird_party/musashi -o build/dasm tools/dasm.c \
 *      third_party/musashi/m68kdasm.c
 *   ./build/dasm re/pipeline/combined.bin 0x213534 0x214730
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "m68k.h"

static uint8_t *image;
static size_t image_len;

unsigned int m68k_read_disassembler_8(unsigned int a)
{ return a < image_len ? image[a] : 0; }
unsigned int m68k_read_disassembler_16(unsigned int a)
{ return (m68k_read_disassembler_8(a) << 8) | m68k_read_disassembler_8(a + 1); }
unsigned int m68k_read_disassembler_32(unsigned int a)
{ return ((unsigned)m68k_read_disassembler_16(a) << 16) |
         m68k_read_disassembler_16(a + 2); }

/* --at reads addresses from stdin, one hex value per line, and prints the
 * single instruction decoded at each.  The recompiler needs a decode at
 * every address the CPU could land on, which is one decode per address;
 * spawning a process per address made that unaffordable, and this makes
 * it one process for all of them. */
static void decode_one(uint32_t pc)
{
    char text[256];
    unsigned n = m68k_disassemble(text, pc, M68K_CPU_TYPE_68000);
    printf("$%06x  %-8s%s\n", pc, "", text);
    (void)n;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[2], "--at")) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        fseek(f, 0, SEEK_END);
        image_len = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        image = malloc(image_len);
        if (fread(image, 1, image_len, f) != image_len) return 1;
        fclose(f);
        char line[64];
        while (fgets(line, sizeof line, stdin)) {
            uint32_t a = (uint32_t)strtoul(line, NULL, 16);
            char text[256];
            unsigned len = m68k_disassemble(text, a, M68K_CPU_TYPE_68000);
            printf("$%06x %u %s\n", a, len, text);
        }
        return 0;
    }
    if (argc != 4) {
        fprintf(stderr, "usage: dasm IMAGE START END | dasm IMAGE --at\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    image_len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    image = malloc(image_len);
    if (fread(image, 1, image_len, f) != image_len) return 1;
    fclose(f);

    uint32_t pc = (uint32_t)strtoul(argv[2], NULL, 0);
    uint32_t end = (uint32_t)strtoul(argv[3], NULL, 0);
    char text[160];
    while (pc < end) {
        unsigned size = m68k_disassemble(text, pc, M68K_CPU_TYPE_68000);
        printf("$%06x  %s\n", pc, text);
        pc += size ? size : 2;
    }
    return 0;
}
