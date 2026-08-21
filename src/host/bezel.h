/* bezel.h -- the Retro Recompilation front-end surround.
 *
 * Two narrow neon side panels with the game between them, laid out
 * inside a 16:9 area centred in the window.  Shared, because the play
 * front end and the RE viewer are two windows onto the same game and
 * looking like two different programs helped nobody.
 */
#ifndef LOTUS2_BEZEL_H
#define LOTUS2_BEZEL_H

#include "raylib.h"

typedef struct {
    Rectangle area;      /* the 16:9 region the bezel occupies */
    Rectangle game;      /* where the caller should draw the game */
    Rectangle left, right;
    Rectangle debug_btn; /* meaningful only when bezel_panels drew one */
} Bezel;

/* Load the logo and the text face.  Safe to call more than once. */
void bezel_assets(void);

void bezel_text(const char *t, int x, int y, int size, Color c);
int  bezel_measure(const char *t, int size);

/* Work out the 16:9 area, the game rectangle at `aw`:`ah`, and the two
 * panel rectangles, inside `win`.  The viewer draws into a fixed logical
 * canvas rather than straight to the window, so the region is passed in
 * rather than read from the screen. */
Bezel bezel_begin(float aw, float ah, Rectangle win);

/* Draw the panels.  `fps` and `natives` fill the build column.  With
 * `want_debug_btn` set a DEBUG button is drawn at the foot of the left
 * panel; the return is 1 on the frame it is clicked. */
int bezel_panels(Bezel *b, int fps, int natives, int want_debug_btn,
                 Vector2 mouse);

#endif
