#ifndef LOTUS2_COMPOSITOR_H
#define LOTUS2_COMPOSITOR_H

#include <stdint.h>

/* Render the display described by the copper list at `coplc` into
 * out[320*200] (host framebuf pixel format).  Returns 0, or -1 if no
 * copper list was found at that address. */
int composite(const uint8_t *chip, uint32_t coplc, uint32_t *out);

int write_ppm_native(const char *path, const uint32_t *fb, int w, int h);

#endif
