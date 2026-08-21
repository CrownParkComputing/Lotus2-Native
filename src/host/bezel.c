/* bezel.c -- see bezel.h.
 *
 * Layout follows ~/Uridium2-Native so the Retro Recompilation front ends
 * look alike: symmetric side panels with neon edges, cyan for player 1
 * and pink for player 2, the game between them.
 *
 * The panels size themselves.  Each is built as a list of rows and then
 * drawn at the largest type size where every line fits the width AND the
 * whole list fits the height, because the window manager decides how big
 * the window is, not us -- fixed sizes clipped the right panel and ran
 * the left one's foot into its body.
 */
#include <stdio.h>
#include <string.h>
#include "bezel.h"

const char *bezel_scaler = "SHARP";

static Font  ui_font;
static int   ui_font_ok;
static Texture2D logo;
static int   assets_done;

/* raylib defines PINK and GOLD as macros, so the palette gets its own
 * names rather than fighting them. */
static const Color P1HUE   = {   0, 225, 255, 255 };
static const Color P2HUE   = { 255,  70, 165, 255 };
static const Color HEADHUE = { 255, 190,  70, 255 };
static const Color BODYHUE = { 196, 200, 212, 255 };

void bezel_assets(void)
{
    if (assets_done) return;
    assets_done = 1;
    if (FileExists("assets/retro_recomp_logo.png")) {
        logo = LoadTexture("assets/retro_recomp_logo.png");
        if (logo.id) SetTextureFilter(logo, TEXTURE_FILTER_BILINEAR);
    }
    static const char *faces[] = {
        "assets/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf", NULL };
    for (int i = 0; faces[i]; i++)
        if (FileExists(faces[i])) {
            ui_font = LoadFontEx(faces[i], 40, NULL, 0);
            ui_font_ok = ui_font.texture.id != 0;
            if (ui_font_ok)
                SetTextureFilter(ui_font.texture, TEXTURE_FILTER_BILINEAR);
            break;
        }
}

void bezel_text(const char *t, int x, int y, int size, Color c)
{
    if (ui_font_ok)
        DrawTextEx(ui_font, t, (Vector2){(float)x, (float)y}, (float)size,
                   0.5f, c);
    else DrawText(t, x, y, size, c);
}

int bezel_measure(const char *t, int size)
{
    if (ui_font_ok)
        return (int)MeasureTextEx(ui_font, t, (float)size, 0.5f).x;
    return MeasureText(t, size);
}

Bezel bezel_begin(float aw, float ah, Rectangle win)
{
    Bezel b = {0};
    int winw = (int)win.width, winh = (int)win.height;
    int w, h;
    /* Pin to 16:9.  On a 5120x1440 ultrawide -- or under a tiling window
     * manager handing out a 1404x306 slot -- stretching to the window
     * shape either pillarboxes absurdly or clips the panels off. */
    if (winw * 9 > winh * 16) { h = winh; w = h * 16 / 9; }
    else { w = winw; h = w * 9 / 16; }
    int ox = (int)win.x + (winw - w) / 2, oy = (int)win.y + (winh - h) / 2;
    b.area = (Rectangle){ox, oy, w, h};

    /* Fill the height, and show the game at its true aspect.  A 320x200
     * lores Amiga screen is not square-pixel -- it was drawn for a 4:3
     * television -- so scaling it 1:1 stretches everything thin.  What
     * width is left over goes to the panels, down to a floor that keeps
     * the legends readable. */
    float gh = h - 24.0f;
    float gw = gh * aw / ah;
    const float min_panel = 150.0f;
    if (gw > w - min_panel * 2) {
        gw = w - min_panel * 2;
        gh = gw * ah / aw;
    }
    b.game = (Rectangle){ox + (w - gw) / 2, oy + (h - gh) / 2, gw, gh};

    int lpx = ox + 10, lpw = (int)b.game.x - lpx - 12;
    int rpx = (int)(b.game.x + gw) + 12, rpw = ox + w - rpx - 10;
    b.left  = (Rectangle){lpx, oy + 10, lpw, h - 20};
    b.right = (Rectangle){rpx, oy + 10, rpw, h - 20};
    return b;
}

int bezel_panels(Bezel *b, int fps, int natives, int want_debug_btn,
                 Vector2 mouse, int right_reserve)
{
    bezel_assets();
    int clicked = 0;
    DrawRectangleLinesEx(b->left, 2, P1HUE);
    DrawRectangleLinesEx(b->right, 2, P2HUE);

    for (int side = 0; side < 2; side++) {
        Rectangle p = side ? b->right : b->left;
        const int px = (int)p.x, pw = (int)p.width;
        const int reserve = side ? right_reserve : 0;
        const int py = (int)p.y + reserve, ph = (int)p.height - reserve;
        Color hue = side ? P2HUE : P1HUE;
        char fpsbuf[32], natbuf[40], scalebuf[48], carbuf[48];
        snprintf(fpsbuf, sizeof fpsbuf, "%d FPS", fps);
        snprintf(natbuf, sizeof natbuf, "%d ROUTINES NATIVE C", natives);

        struct { const char *t; int head; Color c; } rows[26];
        int n = 0;
        #define ROW(T, H, C) do { if (n < 26) { rows[n].t = (T); \
            rows[n].head = (H); rows[n].c = (C); n++; } } while (0)
        ROW("LOTUS TURBO CHALLENGE 2", 0, hue);
        ROW("NATIVE PROJECT", 0, BODYHUE);
        ROW("", 0, BODYHUE);
        ROW(side ? "PLAYER 2" : "PLAYER 1", 1, hue);
        ROW("KEYBOARD", 0, HEADHUE);
        if (side) {
            ROW("W A S D  STEER", 0, BODYHUE);
            ROW("C        ACCELERATE", 0, BODYHUE);
            ROW("V        CHANGE GEAR", 0, BODYHUE);
        } else {
            ROW("ARROWS   STEER", 0, BODYHUE);
            ROW("SPACE    ACCELERATE", 0, BODYHUE);
            ROW("SPACE    CHANGE GEAR", 0, BODYHUE);
            snprintf(carbuf, sizeof carbuf, "F6       CAR: %s",
                     car_hue_name());
            ROW(carbuf, 0, hue);
        }
        ROW("", 0, BODYHUE);
        ROW(side ? "GAMEPAD 2" : "GAMEPAD 1", 0, HEADHUE);
        ROW("D-PAD    STEER", 0, BODYHUE);
        ROW("A / R1 / R2  ACCELERATE", 0, BODYHUE);
        ROW("B / L1 / L2  CHANGE GEAR", 0, BODYHUE);
        ROW(side ? "second pad, port 0" : "first pad, port 1", 0, BODYHUE);
        ROW(side ? "JOYSTICK PORT 0" : "JOYSTICK PORT 1", 0, hue);
        ROW("", 0, BODYHUE);
        if (side) {
            ROW("KEYS", 0, HEADHUE);
            ROW("X        COURSE PREVIEW", 0, hue);
            ROW("         (pad X too)", 0, BODYHUE);
            ROW("F11      FULLSCREEN", 0, BODYHUE);
            snprintf(scalebuf, sizeof scalebuf, "F3       SCALER: %s",
                     bezel_scaler);
            ROW(scalebuf, 0, BODYHUE);
            ROW("P        PAUSE", 0, BODYHUE);
            ROW("F2       SCREENSHOT", 0, BODYHUE);
            ROW("ESC      QUIT", 0, BODYHUE);
        } else {
            /* The passwords, because they are what actually gets you to
             * a course and hunting for them on a bit of paper is worse
             * than any menu I could put in front of you.  FOREST is
             * where the game starts, so it has none. */
            ROW("PASSWORDS", 0, HEADHUE);
            ROW("NIGHT     TWILIGHT", 0, BODYHUE);
            ROW("FOG       PEA SOUP", 0, BODYHUE);
            ROW("SNOW      THE SKIDS", 0, BODYHUE);
            ROW("DESERT    PEACHES", 0, BODYHUE);
            ROW("MOTORWAY  LIVER POOL", 0, BODYHUE);
            ROW("MARSH     BAGLEY", 0, BODYHUE);
            ROW("STORM     E BOW", 0, BODYHUE);
            ROW("", 0, BODYHUE);
            ROW(natbuf, 0, BODYHUE);
            ROW(fpsbuf, 0, hue);
        }
        #undef ROW

        float logo_h = 0;
        if (logo.id) logo_h = (pw - 24.0f) * logo.height / logo.width + 12;
        /* the button, when there is one, is furniture the list has to
         * make room for */
        const int btn_h = want_debug_btn && !side ? 46 : 0;

        int fs = 22;
        for (; fs > 7; fs--) {
            int wide = 0, tall = (int)logo_h + 12 + btn_h;
            for (int i = 0; i < n; i++) {
                int sz = rows[i].head ? fs * 3 / 2 : fs;
                if (bezel_measure(rows[i].t, sz) > pw - 24) wide = 1;
                tall += sz + sz / 2;
            }
            if (!wide && tall < ph - 12) break;
        }

        int x = px + 12, y = py + 10;
        if (logo.id) {
            float lw = pw - 24.0f;
            DrawTexturePro(logo, (Rectangle){0, 0, logo.width, logo.height},
                           (Rectangle){x, y, lw, lw * logo.height / logo.width},
                           (Vector2){0, 0}, 0.0f, WHITE);
            y += (int)(lw * logo.height / logo.width) + 12;
        }
        for (int i = 0; i < n; i++) {
            int sz = rows[i].head ? fs * 3 / 2 : fs;
            if (rows[i].t[0]) bezel_text(rows[i].t, x, y, sz, rows[i].c);
            y += sz + sz / 2;
        }

        if (want_debug_btn && !side) {
            Rectangle btn = {px + 12, py + ph - 56, pw - 24, 40};
            b->debug_btn = btn;
            int hot = CheckCollisionPointRec(mouse, btn);
            DrawRectangleRec(btn, hot ? (Color){40, 70, 90, 255}
                                      : (Color){24, 30, 40, 255});
            DrawRectangleLinesEx(btn, 2, P1HUE);
            int bs = 20, tw = bezel_measure("COURSE PREVIEW  (X)", bs);
            while (tw > btn.width - 10 && bs > 9) {
                bs -= 2;
                tw = bezel_measure("COURSE PREVIEW  (X)", bs);
            }
            bezel_text("COURSE PREVIEW  (X)", (int)(btn.x + (btn.width - tw) / 2),
                       (int)(btn.y + (btn.height - bs) / 2), bs, P1HUE);
            if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) clicked = 1;
        }
    }
    return clicked;
}
