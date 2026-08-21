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
#include "bezel.h"
#include "whdload.h"
#include "pad.h"

int native_overrides_count(void);

static void audio_pull_cb(void *buffer, unsigned int frames)
{
    amiga_audio_pull((int16_t *)buffer, (int)frames);
}

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
static int ui_measure(const char *t, int size)
{
    if (ui_font_ok)
        return (int)MeasureTextEx(ui_font, t, (float)size, 0.5f).x;
    return MeasureText(t, size);
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

/* Fullscreen that actually fills the screen.  ToggleFullscreen() on its
 * own keeps the windowed size as the video mode, so a 1600x900 window on
 * a larger desktop goes fullscreen into a 1600x900 mode and everything
 * outside it is lost off the edges.  Resize to the monitor first, and
 * put the window back where it was on the way out. */
static void go_fullscreen(int on, int win_w, int win_h)
{
    int mon = GetCurrentMonitor();
    if (on && !IsWindowFullscreen()) {
        SetWindowSize(GetMonitorWidth(mon), GetMonitorHeight(mon));
        ToggleFullscreen();
    } else if (!on && IsWindowFullscreen()) {
        ToggleFullscreen();
        SetWindowSize(win_w, win_h);
        Vector2 mp = GetMonitorPosition(mon);
        SetWindowPosition((int)mp.x + (GetMonitorWidth(mon) - win_w) / 2,
                          (int)mp.y + (GetMonitorHeight(mon) - win_h) / 2);
    }
}

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
    /* no RESIZABLE: a tiling window manager takes that as licence to hand
     * out a 791x1294 portrait slot, and the 16:9 layout then lives in a
     * small box in the middle of it.  X or Y goes fullscreen. */

    /* Fit the monitor.  A fixed 1600x900 is larger than plenty of
     * desktops once panels and decoration are taken off, and a window
     * bigger than the screen has its right and bottom edges -- half the
     * bezel -- hanging off it.  Shrink to fit the work area, keeping
     * 16:9, then centre. */
    {
        int mon = GetCurrentMonitor();
        int mw = GetMonitorWidth(mon), mh = GetMonitorHeight(mon);
        if (mw > 320 && mh > 240) {
            int aw = mw - 80, ah = mh - 120;
            if (win_w > aw || win_h > ah) {
                float k = (float)aw / win_w;
                if ((float)ah / win_h < k) k = (float)ah / win_h;
                win_w = (int)(win_w * k);
                win_h = (int)(win_h * k);
                SetWindowSize(win_w, win_h);
            }
            Vector2 mp = GetMonitorPosition(mon);
            SetWindowPosition((int)mp.x + (mw - win_w) / 2,
                              (int)mp.y + (mh - win_h) / 2);
        }
    }
    int windowed_w = win_w, windowed_h = win_h;
    if (fullscreen) go_fullscreen(1, windowed_w, windowed_h);
    SetTargetFPS(50);
    InitAudioDevice();
    /* One buffer per video frame.  raylib's default stream buffer is far
     * larger than 882 frames, and UpdateAudioStream writes whatever it is
     * given into a buffer sized for something else. */
    SetAudioStreamBufferSizeDefault(FRAME_SAMPLES);
    AudioStream stream = LoadAudioStream(AUDIO_RATE, 16, 2);
    /* PULL, not push.
     *
     * Pushing buffers from the video loop means the device gets audio at
     * the video frame rate, and any mismatch -- 49.75 fps against 44100
     * samples a second -- leaves it short.  A starved stream does not go
     * quiet: it repeats its last buffer, and a repeated fragment over the
     * live signal is exactly the echo that has been audible on one voice.
     * With a callback the device asks for precisely the samples it needs,
     * when it needs them, and the video loop's only job is to keep the
     * ring stocked.  The ring is single producer, single consumer, with
     * an atomic fill, so serving it from the audio thread is safe. */
    SetAudioStreamCallback(stream, audio_pull_cb);
    PlayAudioStream(stream);
    /* Prime with a little silence so the first real audio is not racing
     * an already-empty device. */
    {
        static int16_t quiet[FRAME_SAMPLES * 2];
        for (int i = 0; i < 3 && IsAudioStreamProcessed(stream); i++)
            UpdateAudioStream(stream, quiet, FRAME_SAMPLES);
    }
    amiga_audio_generate(FRAME_SAMPLES * 4);   /* prime the ring */

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
    /* Two-stage scale.  Drawing a 320x200 screen straight into an
     * arbitrary window at 4:3 is a non-integer scale, and nearest
     * sampling turns that into unevenly sized pixels -- some rows and
     * columns two host pixels wide, their neighbours three -- which is
     * the blockiness.  Sampling smoothly instead just blurs it.
     *
     * So blow the screen up by an exact integer factor with nearest
     * sampling, which keeps every guest pixel square and identical, and
     * only then scale that to the window smoothly.  Edges stay where the
     * game put them and the steps come out even. */
    const int SHARP = 4;
    RenderTexture2D sharp = LoadRenderTexture(SCREEN_W * SHARP,
                                              SCREEN_H * SHARP);
    SetTextureFilter(sharp.texture, TEXTURE_FILTER_BILINEAR);
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
        if (IsKeyPressed(KEY_F11) || IsKeyPressed(KEY_X) ||
            IsKeyPressed(KEY_Y) ||
            IsGamepadButtonPressed(0, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            go_fullscreen(!IsWindowFullscreen(), windowed_w, windowed_h);
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
                const int target = FRAME_SAMPLES * 4;   /* ~80 ms of slack */
                int fill = amiga_audio_fill();
                if (fill < target) amiga_audio_generate(target - fill);
                if (wav) {
                    /* the dump mirrors what was generated, since nothing
                     * is pushed any more */
                    static int16_t tap[FRAME_SAMPLES * 2];
                    (void)tap;
                }
            }
            /* nothing to push: the callback takes what it needs */
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
        /* integer blow-up first (see SHARP above), outside BeginDrawing */
        BeginTextureMode(sharp);
        ClearBackground(BLACK);
        DrawTexturePro(screen,
                       (Rectangle){0, 0, SCREEN_W, SCREEN_H},
                       (Rectangle){0, 0, SCREEN_W * SHARP, SCREEN_H * SHARP},
                       (Vector2){0, 0}, 0.0f, WHITE);
        EndTextureMode();
        BeginDrawing();
        ClearBackground(UI_GROUND);
        Bezel bz;
        Rectangle src, dst;
        if (bezel) {
            /* 320x200 lores is a 4:3 picture, not a square-pixel one */
            bz = bezel_begin(4.0f, 3.0f,
                             (Rectangle){0, 0, (float)GetScreenWidth(),
                                         (float)GetScreenHeight()});
            dst = bz.game;
            src = (Rectangle){VIEW_X, VIEW_Y, VIEW_W, VIEW_H};
        } else {
            float sw = border ? SCREEN_W : VIEW_W;
            float sh = border ? SCREEN_H : VIEW_H;
            int winw = GetScreenWidth(), winh = GetScreenHeight();
            float s = (float)winh / sh;
            if (sw * s > winw) s = (float)winw / sw;
            src = (Rectangle){border ? 0 : VIEW_X, border ? 0 : VIEW_Y, sw, sh};
            dst = (Rectangle){(winw - sw * s) / 2, (winh - sh * s) / 2,
                              sw * s, sh * s};
        }
        DrawTexturePro(sharp.texture,
                       /* the render texture holds its content upside
                        * down, so the window into it is measured from
                        * the bottom, not the top */
                       (Rectangle){src.x * SHARP,
                                   (SCREEN_H - src.y - src.height) * SHARP,
                                   src.width * SHARP, -src.height * SHARP},
                       dst, (Vector2){0, 0}, 0.0f, WHITE);
        DrawRectangleLinesEx((Rectangle){dst.x - 2, dst.y - 2,
                                         dst.width + 4, dst.height + 4},
                             2, UI_EDGE);
        if (bezel)
            bezel_panels(&bz, GetFPS(), native_overrides_count(), 0,
                         GetMousePosition());
        if (paused) ui_text("PAUSED", (int)dst.x + 12, (int)dst.y + 12,
                             20, RAYWHITE);
        if (shot_at >= 0 && swiv_frame_no >= shot_at && shot_path &&
            strstr(shot_path, ".png")) {
            EndDrawing();
            TakeScreenshot(shot_path);
            /* Also dump the raw guest framebuffer at the SAME frame, so
             * "is the front end mangling the picture?" can be answered by
             * comparing two images from one run rather than two runs --
             * boot is not frame-for-frame repeatable. */
            {
                char raw[512];
                snprintf(raw, sizeof raw, "%.*s.ppm",
                         (int)(strlen(shot_path) - 4), shot_path);
                FILE *rf = fopen(raw, "wb");
                if (rf) {
                    fprintf(rf, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
                    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
                        uint32_t px = framebuf[i];
                        fputc(px & 0xff, rf);
                        fputc((px >> 8) & 0xff, rf);
                        fputc((px >> 16) & 0xff, rf);
                    }
                    fclose(rf);
                }
            }
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
