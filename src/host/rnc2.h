#ifndef LOTUS2_RNC2_H
#define LOTUS2_RNC2_H

#include <stdint.h>

/* RNC2 (Rob Northen compression, method 2) decoder.
 *
 * Returns 0 on success and writes the unpacked size to *out_size, -1
 * if the input is not an RNC2 stream, or another negative value on
 * other decode errors. */
int rnc2_unpack(const uint8_t *input, uint8_t *output,
                uint32_t *out_size);

#endif
