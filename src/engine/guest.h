/* guest.h -- big-endian guest-memory windows for the native Lotus 2 engine.
 *
 * TRANSLATE-stage rule: base-page and chip state live as guest byte images
 * (the same bytes the oracle dumps), so every native routine can be diffed
 * word-for-word against the re/pipeline dumps.  Naming fields comes later;
 * truth stays in the bytes now.
 */
#ifndef LOTUS2_GUEST_H
#define LOTUS2_GUEST_H

#include <stdint.h>
#include <stddef.h>

#define GUEST_CHIP_SIZE  0x80000u   /* CD32 slave asks for 0.5 MB chip */
#define GUEST_BASE_ADDR  0x208000u  /* A3 base page (re/pipeline/bases.json) */
#define GUEST_BASE_SIZE  0x4000u    /* dumped window: $208000..$20c000 */

static inline uint16_t g16(const uint8_t *mem, uint32_t off)
{
    return (uint16_t)((mem[off] << 8) | mem[off + 1]);
}

static inline uint32_t g32(const uint8_t *mem, uint32_t off)
{
    return ((uint32_t)mem[off] << 24) | ((uint32_t)mem[off + 1] << 16) |
           ((uint32_t)mem[off + 2] << 8) | mem[off + 3];
}

static inline void p16(uint8_t *mem, uint32_t off, uint16_t value)
{
    mem[off] = (uint8_t)(value >> 8);
    mem[off + 1] = (uint8_t)value;
}

static inline void p32(uint8_t *mem, uint32_t off, uint32_t value)
{
    p16(mem, off, (uint16_t)(value >> 16));
    p16(mem, off + 2, (uint16_t)value);
}

/* Load a raw dump into a freshly allocated buffer; returns NULL on error
 * or size mismatch (pass expect=0 to accept any size; *size_out gets the
 * actual length). */
uint8_t *guest_load(const char *path, size_t expect, size_t *size_out);

#endif
