/* lotus2_drive.c -- drive the native engine.
 *
 * No 68000, no emulator: this loads a mid-race RAM snapshot as the
 * initial state and then runs ONLY ported C each frame --
 *
 *     input_read -> car chain -> road chain -> road_bands -> composite
 *
 * -- so what you see and steer is the native port of Lotus 2's own code.
 *
 * The parts of the frame that are not ported yet ($21508a's scenery
 * scheduler and the passes after it) simply do not run, so the road is
 * drawn without trees, signs or opponents.  That is a missing feature,
 * not an approximation: every routine that DOES run is byte-exact.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "raylib.h"
#include "engine.h"
#include "compositor.h"

#define A3 0x208000u
#define VIEW (A3 + 0x3054)

static uint32_t framebuf[LOTUS2_SCREEN_W * LOTUS2_SCREEN_H];

/* Build JOYxDAT the way the hardware reports a stick, so the ported
 * decoder sees exactly what it expects. */
static uint16_t joydat(int up, int down, int left, int right)
{
    return (uint16_t)((left << 9) | ((up ^ left) << 8) |
                      (right << 1) | (down ^ right));
}

int main(int argc, char **argv)
{
    const char *fast_path = "re/pipeline/road/ph_0_211e78_fast.bin";
    const char *chip_path = "re/pipeline/road/ph_0_211e78_chip.bin";
    long shot_at = -1;
    const char *shot_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fast") && i + 1 < argc) fast_path = argv[++i];
        else if (!strcmp(argv[i], "--chip") && i + 1 < argc) chip_path = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 2 < argc) {
            shot_at = strtol(argv[++i], NULL, 0);
            shot_path = argv[++i];
        } else {
            fprintf(stderr, "usage: lotus2_drive [--fast F] [--chip F] "
                            "[--shot FRAME PATH]\n");
            return 2;
        }
    }

    Game g = {0};
    size_t len = 0;
    g.fast = guest_load(fast_path, GUEST_FAST_SIZE, &len);
    g.chip = guest_load(chip_path, GUEST_CHIP_SIZE, &len);
    if (!g.fast || !g.chip) return 1;
    g.base = g.fast + (GUEST_BASE_ADDR - GUEST_FAST_ADDR);

    Blitter bl = {0};
    bl.chip = g.chip;
    bl.chip_size = GUEST_CHIP_SIZE;
    bl.bltafwm = bl.bltalwm = 0xffff;

    SetTraceLogLevel(LOG_WARNING);
    InitWindow(LOTUS2_SCREEN_W * 3, LOTUS2_SCREEN_H * 3 + 40,
               "Lotus 2 - native engine");
    SetTargetFPS(50);
    Image img = { .data = framebuf, .width = LOTUS2_SCREEN_W,
                  .height = LOTUS2_SCREEN_H, .mipmaps = 1,
                  .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    Texture2D tex = LoadTextureFromImage(img);

    long frame = 0;
    while (!WindowShouldClose()) {
        /* ---- input: keyboard or pad, through the ported decoder ---- */
        int up = IsKeyDown(KEY_UP), down = IsKeyDown(KEY_DOWN);
        int left = IsKeyDown(KEY_LEFT), right = IsKeyDown(KEY_RIGHT);
        int fire = IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_LEFT_CONTROL);
        if (IsGamepadAvailable(0)) {
            float ax = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
            if (ax < -0.3f) left = 1;
            if (ax > 0.3f) right = 1;
            if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_UP)) up = 1;
            if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) down = 1;
            if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) fire = 1;
        }
        Input in;
        in.joy1dat = joydat(up, down, left, right);
        in.joy0dat = 0;
        in.cia_pra = (uint8_t)(fire ? 0x7f : 0xff);   /* bit 7 low = pressed */

        /* ---- one native frame ---- */
        input_read(&g, &in);
        car_update(&g, VIEW);
        car_checkpoint(&g, VIEW);
        car_clock(&g, VIEW);
        car_distance(&g, VIEW, NULL);
        car_shape(&g, VIEW);
        car_tick(&g, VIEW);
        car_frame_latch(&g);        /* $211dd4 */
        car_latch_gap(&g);          /* the copies with no ported home yet */
        race_frame_publish(&g);     /* $212e58 */
        /* race_frame_begin ($212cea) is ported and gated, but deliberately
         * NOT called here: it rotates the game's TRIPLE buffer, and only
         * the road is being drawn natively so far.  Rotating would show
         * two frames' worth of stale car and HUD.  Pinning one buffer
         * keeps the demo coherent until the rest of the frame is ported. */

        road_sky(&g);
        road_keyframes_near(&g);
        road_interpolate(&g, 0);
        road_band_bounds(&g, VIEW);
        road_perspective_near(&g, f16(&g, VIEW + 0x98));
        road_blitqueue(&g);
        road_bands(&g, &bl, f32(&g, A3 + 0x2f8e), A3 - 0x2bd8, A3 - 0x4180,
                   f16(&g, A3 + 0x30e4), f16(&g, A3 + 0x30ec),
                   f16(&g, A3 + 0x30dc), f32(&g, A3 + 0x30d8), 0x2c);

        /* the game shows the buffer at $2f8a while drawing into $2f8e */
        composite(g.chip, 0x7ed0c, framebuf);
        UpdateTexture(tex, framebuf);

        BeginDrawing();
        ClearBackground(BLACK);
        DrawTexturePro(tex,
                       (Rectangle){0, 0, LOTUS2_SCREEN_W, LOTUS2_SCREEN_H},
                       (Rectangle){0, 0, LOTUS2_SCREEN_W * 3,
                                   LOTUS2_SCREEN_H * 3},
                       (Vector2){0, 0}, 0.0f, WHITE);
        char hud[160];
        snprintf(hud, sizeof hud,
                 "native engine   frame %ld   seg %u   speed %d   gear %d",
                 frame, (unsigned)(f32(&g, A3 + 0x30d8) >> 16),
                 (int16_t)f16(&g, VIEW + 0x0e),
                 (int16_t)f16(&g, VIEW + 0x28));
        DrawText(hud, 8, LOTUS2_SCREEN_H * 3 + 12, 18, RAYWHITE);
        EndDrawing();

        if (shot_at >= 0 && frame >= shot_at) {
            Image s = LoadImageFromScreen();
            ExportImage(s, shot_path);
            UnloadImage(s);
            fprintf(stderr, "lotus2_drive: wrote %s at frame %ld\n",
                    shot_path, frame);
            break;
        }
        frame++;
    }
    UnloadTexture(tex);
    CloseWindow();
    return 0;
}
