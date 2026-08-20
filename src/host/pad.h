#ifndef SWIV_PAD_H
#define SWIV_PAD_H

#include <stdbool.h>
#include <stdint.h>
#include "raylib.h"

/* A stick state in joy_state bit order: 0 up, 1 down, 2 left, 3 right,
 * 4 fire, 5 second button. */

void    js_poll(void);                     /* read /dev/input/js* */
bool    js_present(int pad);
bool    js_button_down(int pad, int button);

uint8_t keyboard_stick(bool arrows);       /* arrows+space, or WASD */
uint8_t gamepad_stick(int pad);            /* direct node, else raylib */

void      map_raw_key(int key, uint8_t raw);   /* host key -> Amiga rawcode */
Rectangle fit_screen(void);                    /* letterboxed 320x256 */

#endif
