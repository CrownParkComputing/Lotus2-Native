/* Player input shared by every raylib frontend in this tree.
 *
 * Nothing here is title specific: a stick state is the four directions plus
 * two buttons, in the bit order amiga.c's joy_state uses.  Which port each
 * device feeds, and what the buttons mean, is the frontend's business. */
#include "pad.h"

#include "raylib.h"

#include <stdio.h>

/* ------------------------------------------------------------------------
 * Direct Linux joystick input.
 *
 * raylib's GLFW backend enumerated the pad ("Xbox Wireless Controller") and
 * returned its mapped state -- the triggers correctly sat at -1.0 -- but never
 * reported a single button or stick movement, so nothing reached the game.
 * The kernel is happy with the device (EV=20001b with the full BTN_A..BTN_THUMBR
 * range, one js0 node, user in the input group), so read it ourselves instead
 * of depending on GLFW's gamepad mapping database.
 *
 * The classic joystick API is tiny and stable: 8-byte events, type 1 buttons,
 * type 2 axes, bit 0x80 marking the initial state burst on open.  Xbox layout:
 * buttons 0=A 1=B 2=X 3=Y 4=LB 5=RB 6=Back 7=Start; axes 0/1 left stick,
 * 6/7 the d-pad.
 */
#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

struct js_event_packet { uint32_t time; int16_t value; uint8_t type; uint8_t number; };

#define JS_MAX_PADS 2
static int js_fd[JS_MAX_PADS] = {-1, -1};
static uint8_t js_button[JS_MAX_PADS][16];
static int16_t js_axis[JS_MAX_PADS][8];
static bool js_ready[JS_MAX_PADS];

void js_poll(void)
{
    for (int pad = 0; pad < JS_MAX_PADS; pad++) {
        if (js_fd[pad] < 0) {
            char path[32];
            snprintf(path, sizeof path, "/dev/input/js%d", pad);
            js_fd[pad] = open(path, O_RDONLY | O_NONBLOCK);
            if (js_fd[pad] < 0) continue;
            fprintf(stderr, "joystick: opened %s directly\n", path);
        }
        struct js_event_packet event;
        while (read(js_fd[pad], &event, sizeof event) == (ssize_t)sizeof event) {
            uint8_t kind = event.type & ~0x80u;
            if (kind == 1 && event.number < 16) {
                js_button[pad][event.number] = event.value ? 1 : 0;
                if (!(event.type & 0x80)) {
                    static int shown;
                    if (shown < 12)
                        fprintf(stderr, "joystick: pad %d button %u %s\n",
                                pad, event.number,
                                event.value ? "down" : "up"), shown++;
                    js_ready[pad] = true;
                }
            } else if (kind == 2 && event.number < 8) {
                js_axis[pad][event.number] = event.value;
                if (!(event.type & 0x80) &&
                    (event.value > 16000 || event.value < -16000))
                    js_ready[pad] = true;
            }
        }
    }
}

bool js_present(int pad)
{
    return pad >= 0 && pad < JS_MAX_PADS && js_fd[pad] >= 0;
}

static uint8_t js_stick(int pad)
{
    if (!js_present(pad)) return 0;
    uint8_t state = 0;
    int16_t x = js_axis[pad][0], y = js_axis[pad][1];
    int16_t dx = js_axis[pad][6], dy = js_axis[pad][7];
    if (y < -12000 || dy < -12000) state |= 0x01;
    if (y >  12000 || dy >  12000) state |= 0x02;
    if (x < -12000 || dx < -12000) state |= 0x04;
    if (x >  12000 || dx >  12000) state |= 0x08;
    if (js_button[pad][0] || js_button[pad][5]) state |= 0x10;  /* A / RB */
    if (js_button[pad][1] || js_button[pad][4]) state |= 0x20;  /* B / LB */
    return state;
}

bool js_button_down(int pad, int button)
{
    return js_present(pad) && button < 16 && js_button[pad][button];
}
#else
void js_poll(void) {}
bool js_present(int pad) { (void)pad; return false; }
static uint8_t js_stick(int pad) { (void)pad; return 0; }
bool js_button_down(int pad, int b) { (void)pad; (void)b; return false; }
#endif

uint8_t keyboard_stick(bool arrows)
{
    uint8_t state = 0;
    if (IsKeyDown(arrows ? KEY_UP : KEY_W)) state |= 0x01;
    if (IsKeyDown(arrows ? KEY_DOWN : KEY_S)) state |= 0x02;
    if (IsKeyDown(arrows ? KEY_LEFT : KEY_A)) state |= 0x04;
    if (IsKeyDown(arrows ? KEY_RIGHT : KEY_D)) state |= 0x08;
    if (arrows) {
        if (IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_LEFT_CONTROL) ||
            IsKeyDown(KEY_ENTER)) state |= 0x10;
        if (IsKeyDown(KEY_X) || IsKeyDown(KEY_LEFT_SHIFT)) state |= 0x20;
    } else {
        if (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_C)) state |= 0x10;
        if (IsKeyDown(KEY_V) || IsKeyDown(KEY_TAB)) state |= 0x20;
    }
    return state;
}

uint8_t gamepad_stick(int pad)
{
    /* The direct joystick node wins when it is open; raylib stays as the
     * fallback for platforms without /dev/input/js*. */
    if (js_present(pad)) return js_stick(pad);
    if (!IsGamepadAvailable(pad)) return 0;
    uint8_t state = 0;
    float x = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X);
    float y = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y);
    if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_UP) || y < -0.35f)
        state |= 0x01;
    if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_DOWN) || y > 0.35f)
        state |= 0x02;
    if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_LEFT) || x < -0.35f)
        state |= 0x04;
    if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || x > 0.35f)
        state |= 0x08;
    /* Two buttons, as on the original stick: A shoots, B drops a smart bomb.
     * The shoulder buttons mirror them for comfort.  START is deliberately
     * NOT a fire alias any more -- it is the one-player start below, and
     * having it also shoot made the two indistinguishable. */
    if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) ||
        IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
        state |= 0x10;
    if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
        IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
        state |= 0x20;
    static uint8_t previous[4];
    if (pad < 4 && state != previous[pad]) {
#ifdef PLATFORM_ANDROID
        TraceLog(LOG_WARNING, "ANDROID: controller %d state=$%02x axes=(%.2f,%.2f)",
                 pad, state, x, y);
#else
        fprintf(stderr, "controller %d state=$%02x axes=(%.2f,%.2f)\n",
                pad, state, x, y);
#endif
        previous[pad] = state;
    }
    return state;
}

/* An Amiga pixel is not square: a PAL screen fills a 4:3 display, so the
 * buffer has to be presented at 4:3 rather than at its own 352:288.  Showing
 * it 1:1 squashes everything horizontally by about a ninth. */
#define DISPLAY_ASPECT (4.0f / 3.0f)

Rectangle fit_screen(void)
{
    float scale_x = GetScreenWidth() / (SCREEN_H * DISPLAY_ASPECT);
    float scale_y = GetScreenHeight() / (float)SCREEN_H;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    float width = SCREEN_H * DISPLAY_ASPECT * scale;
    float height = SCREEN_H * scale;
    return (Rectangle){(GetScreenWidth() - width) * 0.5f,
                       (GetScreenHeight() - height) * 0.5f,
                       width, height};
}

void map_raw_key(int key, uint8_t raw)
{
    if (IsKeyPressed(key)) amiga_key_event(raw, false);
    if (IsKeyReleased(key)) amiga_key_event(raw, true);
}

