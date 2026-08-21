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

int native_overrides_count(void);

/* Layout and palette follow ~/Uridium2-Native/src/viewer.c so the Retro
 * Recomp front ends look like one family: 1600x900, DejaVuSans, a near
 * black ground, and the logo bilinear-filtered rather than point-sampled. */
static Font ui_font;
static int ui_font_ok;
static void ui_text(const char *t, int x, int y, int size, Color c)
{
    if (ui_font_ok)
        DrawTextEx(ui_font, t, (Vector2){(float)x, (float)y}, (float)size,
                   0.5f, c);
    else
        DrawText(t, x, y, size, c);   /* raylib's own, not this wrapper */
}
#define UI_GROUND  (Color){ 12, 12, 16, 255 }
#define UI_LABEL   (Color){ 110, 122, 145, 255 }
#define UI_TEXT    (Color){ 170, 175, 190, 255 }
#define UI_ACCENT  (Color){ 140, 200, 160, 255 }
#define UI_EDGE    (Color){ 44, 46, 58, 255 }

/* The game programs a 320x200 display inside the host's 352x288 buffer,
 * at (17,18) -- the same offset the compositor gates match at.  The rest
 * is dead border, and scaling it wastes most of the window's height.
 * Every screen's content sits inside this rectangle, menus included. */
#define VIEW_X 17
#define VIEW_Y 18
#define VIEW_W 320
#define VIEW_H 200

#define AUDIO_RATE 44100
#define FRAME_SAMPLES (AUDIO_RATE / 50)

int main(int argc, char **argv)
{
    WhdConfig whd = { .dir = "original/Lotus2CD32",
                      .slave = "Lotus2CD32.slave" };
    int scale = 3, fullscreen = 0, border = 0, bezel = 1;
    long shot_at = -1; const char *shot_path = NULL;
    const char *wav_path = NULL, *rec_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) whd.dir = argv[++i];
        else if (!strcmp(argv[i], "--slave") && i + 1 < argc) whd.slave = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--border")) border = 1;   /* old framing */
        else if (!strcmp(argv[i], "--nobezel")) bezel = 0;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) rec_path = argv[++i];
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
    /* 16:9 by default, with the 4:3 game centred and side panels either
     * side; --nobezel gives just the game at VIEW_W x VIEW_H * scale. */
    int win_w = bezel ? 1600 : VIEW_W * scale;
    int win_h = bezel ? 900  : VIEW_H * scale;
    InitWindow(win_w, win_h, "Lotus Turbo Challenge 2 -- native");
    SetWindowState(FLAG_WINDOW_RESIZABLE);
    if (fullscreen) ToggleFullscreen();
    SetTargetFPS(50);
    InitAudioDevice();
    /* One buffer per video frame.  raylib's default stream buffer is far
     * larger than 882 frames, and UpdateAudioStream writes whatever it is
     * given into a buffer sized for something else. */
    SetAudioStreamBufferSizeDefault(FRAME_SAMPLES);
    AudioStream stream = LoadAudioStream(AUDIO_RATE, 16, 2);
    PlayAudioStream(stream);
    /* Prime with a little silence so the first real audio is not racing
     * an already-empty device. */
    {
        static int16_t quiet[FRAME_SAMPLES * 2];
        for (int i = 0; i < 3 && IsAudioStreamProcessed(stream); i++)
            UpdateAudioStream(stream, quiet, FRAME_SAMPLES);
    }
    /* Prime the device with a little silence so the first frames of real
     * audio are not racing an already-empty buffer. */
    {
        static int16_t quiet[FRAME_SAMPLES * 2];
        for (int i = 0; i < 3 && IsAudioStreamProcessed(stream); i++)
            UpdateAudioStream(stream, quiet, FRAME_SAMPLES);
    }

    Texture2D logo = { 0 };
    if (bezel && FileExists("assets/retro_recomp_logo.png")) {
        logo = LoadTexture("assets/retro_recomp_logo.png");
        if (logo.id) SetTextureFilter(logo, TEXTURE_FILTER_BILINEAR);
    }
    if (FileExists("assets/DejaVuSans.ttf")) {
        ui_font = LoadFontEx("assets/DejaVuSans.ttf", 40, NULL, 0);
        ui_font_ok = ui_font.texture.id != 0;
        if (ui_font_ok) SetTextureFilter(ui_font.texture, TEXTURE_FILTER_BILINEAR);
    }

    Image image = { .data = framebuf, .width = SCREEN_W, .height = SCREEN_H,
                    .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    Texture2D screen = LoadTextureFromImage(image);
    static int16_t mix[FRAME_SAMPLES * 2];
    int paused = 0;
    /* --wav writes the mixed stereo stream as raw s16le, so "is there
     * sound?" is a question with an answer rather than a listening test. */
    FILE *wav = wav_path ? fopen(wav_path, "wb") : NULL;
    /* --record writes one input byte per frame.  Automated input cannot
     * get past a password screen, so the only way to reach courses 2-8 is
     * for a person to play there -- and a recording turns that play into
     * a reproducible test the oracle can be run against. */
    FILE *rec = rec_path ? fopen(rec_path, "wb") : NULL;

    while (!WindowShouldClose() && !amiga_stopped()) {
        if (IsKeyPressed(KEY_P)) paused = !paused;
        if (IsKeyPressed(KEY_F11) || IsKeyPressed(KEY_X) || IsKeyPressed(KEY_Y))
            ToggleFullscreen();
        if (IsKeyPressed(KEY_F2)) TakeScreenshot("lotus2.png");

        if (!paused) {
            js_poll();
            /* both ports get the same stick: the game reads port 1 for
             * the near car and port 0 for the menus */
            uint8_t stick = keyboard_stick(true) | gamepad_stick(0);
            joy_state[0] = joy_state[1] = stick;
            if (rec) fputc(stick, rec);
            amiga_run_frame();
            /* Paula is mixed into the host's ring here; without this call
             * the ring stays empty and amiga_audio_pull dutifully returns
             * silence, which looks exactly like working audio from the
             * chipset's side -- channels on, periods and volumes set, and
             * nothing coming out. */
            /* Keep the ring at a steady level by generating exactly the
             * shortfall in SAMPLES.  One 882-sample frame per video frame
             * assumes 50.000 fps; the real rate is 49.75, so the device
             * is short a couple of hundred samples a second and repeats
             * its last buffer to cover -- which is the reverb.  Topping
             * up a whole frame at a time overcorrects just as badly in
             * the other direction. */
            {
                const int target = FRAME_SAMPLES * 3;   /* ~60 ms of slack */
                int fill = amiga_audio_fill();
                if (fill < target) amiga_audio_generate(target - fill);
            }
            /* Drain every buffer the device will take, not one per video
             * frame.  Feeding exactly one and only when the stream
             * happened to be ready left roughly one frame in forty
             * unqueued: the device starved, and the gap is heard as a
             * click at a frame boundary. */
            while (IsAudioStreamProcessed(stream)) {
                if (amiga_audio_fill() < FRAME_SAMPLES) break;  /* no full
                            frame ready: better to wait than to feed silence */
                amiga_audio_pull(mix, FRAME_SAMPLES);
                UpdateAudioStream(stream, mix, FRAME_SAMPLES);
                if (wav) fwrite(mix, sizeof(int16_t), FRAME_SAMPLES * 2, wav);
            }
        }

        if (shot_at >= 0 && swiv_frame_no >= shot_at && shot_path &&
            !strstr(shot_path, ".png")) {
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
        ClearBackground(UI_GROUND);
        /* Lay the whole bezel out inside a 16:9 area centred in the
         * window.  On a 5120x1440 ultrawide -- or under a tiling WM that
         * hands out a 1404x306 slot -- stretching to the window shape
         * either pillarboxes absurdly or clips the panels off the bottom.
         * Pinning to 16:9 keeps the framing the same everywhere. */
        int winw = GetScreenWidth(), winh = GetScreenHeight();
        int w = winw, h = winh, ox = 0, oy = 0;
        if (bezel) {
            if (winw * 9 > winh * 16) { h = winh; w = h * 16 / 9; }
            else { w = winw; h = w * 9 / 16; }
            ox = (winw - w) / 2;
            oy = (winh - h) / 2;
        }
        float sw = border ? SCREEN_W : VIEW_W;
        float sh = border ? SCREEN_H : VIEW_H;

        /* The game is 4:3; the window is 16:9.  Fill the height with the
         * game and give the leftover width to the two panels. */
        float s = (float)h / sh;
        float gw = sw * s * (4.0f / 3.0f) / (sw / sh) / (4.0f / 3.0f);
        gw = sw * s;
        if (bezel) {
            float want = h * 4.0f / 3.0f;        /* 4:3 at full height */
            if (want > w * 0.72f) want = w * 0.72f;
            s = want / sw;
            gw = want;
        }
        if (gw > w) { s = (float)w / sw; gw = w; }
        float gh = sh * s;
        Rectangle src = { border ? 0 : VIEW_X, border ? 0 : VIEW_Y, sw, sh };
        Rectangle dst = { ox + (w - gw) / 2, oy + (h - gh) / 2, gw, gh };
        DrawTexturePro(screen, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
        DrawRectangleLinesEx((Rectangle){dst.x - 2, dst.y - 2, gw + 4, gh + 4},
                             2, UI_EDGE);

        if (bezel) {
            const Color LBL = UI_LABEL;
            const Color VAL = UI_TEXT;
            const Color HDR = UI_ACCENT;
            int panel = (int)dst.x - ox;          /* width of the left panel */
            int rx = (int)(dst.x + gw) + 18;
            int rw = ox + w - rx - 18;
            const int lx = ox + 20;               /* left panel text column */
            const int ty = oy + 24;               /* top of both panels */

            /* left: the logo, then player 1's controls */
            if (logo.id) {
                float lw = panel - 36.0f;
                if (lw > 300) lw = 300;
                float lh = lw * logo.height / logo.width;
                DrawTexturePro(logo,
                    (Rectangle){0, 0, logo.width, logo.height},
                    (Rectangle){ox + 18 + (panel - 36 - lw) / 2, ty, lw, lh},
                    (Vector2){0, 0}, 0.0f, WHITE);
            }
            int y = ty + 110;
            ui_text("PLAYER 1", lx, y, 20, HDR); y += 28;
            ui_text("steer      arrow keys", lx, y, 16, LBL); y += 20;
            ui_text("accelerate space", lx, y, 16, LBL); y += 20;
            ui_text("gear       space (hold)", lx, y, 16, LBL); y += 20;
            ui_text("joystick   port 1", lx, y, 16, VAL); y += 34;
            ui_text("PLAYER 2", lx, y, 20, HDR); y += 28;
            ui_text("joystick   port 0", lx, y, 16, LBL); y += 20;
            ui_text("(two-player not", lx, y, 16, LBL); y += 18;
            ui_text(" wired up yet)", lx, y, 16, LBL);

            /* right: what this actually is */
            y = ty;
            ui_text("LOTUS TURBO", rx, y, 22, VAL); y += 24;
            ui_text("CHALLENGE 2", rx, y, 22, VAL); y += 30;
            ui_text("Gremlin, 1991", rx, y, 16, LBL); y += 20;
            ui_text("Magnetic Fields", rx, y, 16, LBL); y += 32;
            ui_text("NATIVE BUILD", rx, y, 18, HDR); y += 24;
            ui_text("68000 recompiled to C", rx, y, 15, LBL); y += 18;
            ui_text("no emulator linked in", rx, y, 15, LBL); y += 18;
            {
                char b[64];
                snprintf(b, sizeof b, "%d routines native C",
                         native_overrides_count());
                ui_text(b, rx, y, 15, LBL); y += 18;
                snprintf(b, sizeof b, "%d fps", GetFPS());
                ui_text(b, rx, y, 15, VAL); y += 30;
            }
            ui_text("KEYS", rx, y, 18, HDR); y += 24;
            ui_text("X / Y   fullscreen", rx, y, 15, LBL); y += 18;
            ui_text("P       pause", rx, y, 15, LBL); y += 18;
            ui_text("F2      screenshot", rx, y, 15, LBL); y += 18;
            ui_text("Esc     quit", rx, y, 15, LBL);
            (void)rw;
        }
        if (paused) ui_text("PAUSED", (int)dst.x + 12, (int)dst.y + 12,
                             20, RAYWHITE);
        if (shot_at >= 0 && swiv_frame_no >= shot_at && shot_path &&
            strstr(shot_path, ".png")) {
            EndDrawing();
            TakeScreenshot(shot_path);
            fprintf(stderr, "lotus2_game: wrote %s at frame %ld\n",
                    shot_path, swiv_frame_no);
            break;
        }
        EndDrawing();
    }

    if (rec) { fprintf(stderr, "lotus2_game: recorded %ld frames of input "
                       "to %s\n", swiv_frame_no, rec_path); fclose(rec); }
    if (wav) fclose(wav);
    UnloadAudioStream(stream);
    CloseAudioDevice();
    CloseWindow();
    amiga_report();
    return 0;
}
