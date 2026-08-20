/* lotus2_game.c -- play Lotus Turbo Challenge 2 natively.
 *
 * Not a debug tool.  The viewer in src/viewer/ boots the game only to
 * freeze it once the course data lands, and defaults to a top-down map
 * page; it was never a way to play.  This is: run the host frame loop,
 * put the game's own framebuffer on screen, feed it the pad, and play
 * the audio.  The CPU behind it is recompiled C -- nothing from
 * third_party/musashi is linked in.
 *
 * Controls: arrow keys or a gamepad; space / button 1 = fire;
 *           F11 fullscreen, F2 screenshot, P pause, Esc quit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "raylib.h"
#include "amiga.h"
#include "cpu.h"
#include "whdload.h"
#include "pad.h"

#define AUDIO_RATE 44100
#define FRAME_SAMPLES (AUDIO_RATE / 50)

int main(int argc, char **argv)
{
    WhdConfig whd = { .dir = "original/Lotus2CD32",
                      .slave = "Lotus2CD32.slave" };
    int scale = 3, fullscreen = 0;
    long shot_at = -1; const char *shot_path = NULL;
    const char *wav_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) whd.dir = argv[++i];
        else if (!strcmp(argv[i], "--slave") && i + 1 < argc) whd.slave = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 2 < argc) {
            shot_at = strtol(argv[++i], NULL, 0);
            shot_path = argv[++i];
        }
        else {
            fprintf(stderr, "usage: lotus2_game [--dir DIR] [--scale N] "
                            "[--fullscreen]\n");
            return 2;
        }
    }

    amiga_init();
    /* Without this the chipset runs but never rasterises: the game plays
     * perfectly into a framebuffer that is never written, and the window
     * is black.  The headless runner turns it on from its own flags, so
     * a front end that forgets it looks exactly like a game that will not
     * boot. */
    amiga_enable_video(true);
    if (!whdload_boot(&whd)) {
        fprintf(stderr, "lotus2: the slave did not boot\n");
        return 1;
    }

    SetTraceLogLevel(LOG_WARNING);
    InitWindow(SCREEN_W * scale, SCREEN_H * scale,
               "Lotus Turbo Challenge 2 -- native");
    if (fullscreen) ToggleFullscreen();
    SetTargetFPS(50);
    InitAudioDevice();
    /* One buffer per video frame.  raylib's default stream buffer is far
     * larger than 882 frames, and UpdateAudioStream writes whatever it is
     * given into a buffer sized for something else. */
    SetAudioStreamBufferSizeDefault(FRAME_SAMPLES);
    AudioStream stream = LoadAudioStream(AUDIO_RATE, 16, 2);
    PlayAudioStream(stream);

    Image image = { .data = framebuf, .width = SCREEN_W, .height = SCREEN_H,
                    .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    Texture2D screen = LoadTextureFromImage(image);
    static int16_t mix[FRAME_SAMPLES * 2];
    int paused = 0;
    /* --wav writes the mixed stereo stream as raw s16le, so "is there
     * sound?" is a question with an answer rather than a listening test. */
    FILE *wav = wav_path ? fopen(wav_path, "wb") : NULL;

    while (!WindowShouldClose() && !amiga_stopped()) {
        if (IsKeyPressed(KEY_P)) paused = !paused;
        if (IsKeyPressed(KEY_F11)) ToggleFullscreen();
        if (IsKeyPressed(KEY_F2)) TakeScreenshot("lotus2.png");

        if (!paused) {
            js_poll();
            /* both ports get the same stick: the game reads port 1 for
             * the near car and port 0 for the menus */
            uint8_t stick = keyboard_stick(true) | gamepad_stick(0);
            joy_state[0] = joy_state[1] = stick;
            amiga_run_frame();
            /* Paula is mixed into the host's ring here; without this call
             * the ring stays empty and amiga_audio_pull dutifully returns
             * silence, which looks exactly like working audio from the
             * chipset's side -- channels on, periods and volumes set, and
             * nothing coming out. */
            amiga_audio_frame();
            if (IsAudioStreamProcessed(stream)) {
                amiga_audio_pull(mix, FRAME_SAMPLES);   /* zero-fills short */
                UpdateAudioStream(stream, mix, FRAME_SAMPLES);
                if (wav) fwrite(mix, sizeof(int16_t), FRAME_SAMPLES * 2, wav);
            }
        }

        if (shot_at >= 0 && swiv_frame_no >= shot_at) {
            FILE *f = fopen(shot_path, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
                for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
                    uint32_t p = framebuf[i];
                    fputc(p & 0xff, f);
                    fputc((p >> 8) & 0xff, f);
                    fputc((p >> 16) & 0xff, f);
                }
                fclose(f);
                fprintf(stderr, "lotus2_game: wrote %s at frame %ld\n",
                        shot_path, swiv_frame_no);
            }
            break;
        }
        UpdateTexture(screen, framebuf);
        BeginDrawing();
        ClearBackground(BLACK);
        int w = GetScreenWidth(), h = GetScreenHeight();
        float s = (float)w / SCREEN_W;
        if ((float)h / SCREEN_H < s) s = (float)h / SCREEN_H;
        Rectangle src = { 0, 0, SCREEN_W, SCREEN_H };
        Rectangle dst = { (w - SCREEN_W * s) / 2, (h - SCREEN_H * s) / 2,
                          SCREEN_W * s, SCREEN_H * s };
        DrawTexturePro(screen, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
        if (paused) DrawText("PAUSED", 12, 12, 20, RAYWHITE);
        EndDrawing();
    }

    if (wav) fclose(wav);
    UnloadAudioStream(stream);
    CloseAudioDevice();
    CloseWindow();
    amiga_report();
    return 0;
}
