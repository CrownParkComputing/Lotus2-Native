/* lotus2_view.c -- Lotus 2 live viewer + RE debug pages (raylib).
 *
 * House style follows SWIV-Native's swivview: a fixed logical canvas
 * blitted scaled+centred, the game view on top, a full-width dark control
 * bar underneath with immediate-mode buttons, and full-screen debug pages
 * reached from those buttons.  Controls are on-screen; keys are extras.
 *
 * Pages:
 *   PLAY     the game, driven by keyboard/pad
 *   COURSE   each course previewed in 3D by the game's own road chain,
 *            beside a top-down map, a gradient profile and a strip
 *   GRAPHICS a planar bitmap viewer over chip RAM -- the loading and
 *            course-intro pictures, decoded with the live palette
 *   SOUND    the four Paula voices, their registers and their samples
 *
 * Keys: F2 screenshot, P pause, ESC back to PLAY, arrows+space drive.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include "raylib.h"
#include "amiga.h"
#include "whdload.h"
#include "pad.h"
#include "cpu.h"
#include "bezel.h"

/* Sound, on the same pull model as the play front end: the device asks
 * for exactly the samples it needs and the video loop only keeps the
 * ring stocked.  Pushing a buffer per video frame starves the device --
 * 49.75 fps against 44100 samples a second is short every frame -- and a
 * starved stream repeats its last buffer, which is audible as an echo on
 * whichever voice is loudest. */
#define AUDIO_RATE 44100
#define FRAME_SAMPLES (AUDIO_RATE / 50)
static void audio_pull_cb(void *buffer, unsigned int frames)
{
    amiga_audio_pull((int16_t *)buffer, (int)frames);
}
#include "engine.h"
#include "compositor.h"

#define A3 0x208000u

#define VIEW_W 320
#define VIEW_H 200
#define BAR_H  140
#define WIN_W  1920
#define WIN_H  1080

/* The course table: 1024 16-byte records at A3-$1e7d.  The generator at
 * $213edc walks it with `adda.w #$10,a0` then reads (-$b,a0) = curvature
 * delta and (-$a,a0) = slope delta.  The car's segment is the HIGH word
 * of the long at $30d8(A3) (the code does `swap d0; asl.w #4`). */
/* The race window sits at raster (17,18) in the host's 352x288 frame --
 * measured by the render gate, which proves that window is pixel-identical
 * to the native compositor's output. */
#define GAME_OX 17
#define GAME_OY 18
#define GAME_W  320
#define GAME_H  200

/* Offline course file (tools/course_extract.py).  The debug section does
 * not need to boot the game and drive into a race: a course is 16 KB of
 * table plus the palette it is drawn with, so the viewer opens that
 * directly and starts instantly.  With no file it falls back to reading a
 * live session's memory. */
#define COURSE_MAGIC 0x3143324cu       /* "L2C1" little-endian */
static uint8_t *course_file;           /* NULL = read the live game */
static uint32_t course_file_id;
static char course_file_name[32];
static Color course_file_sky, course_file_grass, course_file_road;
/* The eight levels, named from the retail Codes file (the passwords are
 * how you reach them in the game).  A course is available to the debug
 * viewer once it has been captured to re/pipeline/course_<name>.l2c. */
typedef struct { const char *name, *code, *file; } CourseEntry;
/* Level order and passwords as the game presents them (PASSWORDS on the
 * title screen); the name of each level comes from the retail Codes file. */
static const CourseEntry COURSES[] = {
    {"FOREST",   "-",          "re/pipeline/course_forest.l2c"},
    {"NIGHT",    "TWILIGHT",   "re/pipeline/course_night.l2c"},
    {"FOG",      "PEA SOUP",   "re/pipeline/course_fog.l2c"},
    {"SNOW",     "THE SKIDS",  "re/pipeline/course_snow.l2c"},
    {"DESERT",   "PEACHES",    "re/pipeline/course_desert.l2c"},
    {"MOTORWAY", "LIVER POOL", "re/pipeline/course_motorway.l2c"},
    {"MARSH",    "BAGLEY",     "re/pipeline/course_marsh.l2c"},
    {"STORM",    "E BOW",      "re/pipeline/course_storm.l2c"},
};
#define COURSE_COUNT ((int)(sizeof COURSES / sizeof COURSES[0]))
static int course_sel;

#define COURSE_FILE_TABLE 54           /* 4 magic + 4 segs + 4 id + 32 name + 9 colour + 1 pad */

#define COURSE_BASE    (A3 - 0x1e7d)
#define COURSE_SEGMENTS 1024
#define COURSE_RECORD   0x10

/* Two pages: the game, and the course preview (3D road + 2D map,
 * profile and strip).  The TRACK, GEOM and DISPLAY pages were
 * scaffolding for porting the render chain; that work is done and
 * they are gone. */
enum { MODE_GAME, MODE_COURSE, MODE_GFX, MODE_SOUND, MODE_COUNT };

/* The game itself is the front page.  This tool used to boot the game
 * only to freeze it the moment the course data landed, which is useful
 * for reading state and useless for playing; now it plays by default and
 * the debug pages are a key away. */
static void page_game(void);


/* ---- house palette (swivview) ---- */
static const Color BAR_BG   = {28, 28, 34, 255};
static const Color BTN_IDLE = {50, 50, 58, 255};
static const Color BTN_HOT  = {80, 80, 90, 255};
static const Color BTN_ON   = {70, 130, 200, 255};
static const Color BTN_EDGE = {120, 120, 130, 255};
static const Color LABEL    = {255, 238, 136, 255};
static const Color PANEL_BG = {20, 20, 26, 255};
static const Color GRID     = {50, 50, 58, 255};

/* ---- logical canvas + input mapping (swivview convention) ---- */
static RenderTexture2D canvas_rt;
static float view_scale = 1.0f, view_ox = 0, view_oy = 0;
static Vector2 to_canvas(Vector2 p)
{ return (Vector2){(p.x - view_ox) / view_scale, (p.y - view_oy) / view_scale}; }
static Vector2 mpos(void) { return to_canvas(GetMousePosition()); }

static Font ui_font;
static int ui_font_ok;
static void ui_text(const char *t, int x, int y, int fs, Color c)
{
    if (ui_font_ok)
        DrawTextEx(ui_font, t, (Vector2){(float)x, (float)y}, (float)fs, 1, c);
    else DrawText(t, x, y, fs, c);
}
static int ui_measure(const char *t, int fs)
{
    return ui_font_ok ? (int)MeasureTextEx(ui_font, t, (float)fs, 1).x
                      : MeasureText(t, fs);
}
static int ui_input = 1;   /* cleared in --shot mode so captures are deterministic */
static int ui_hit(Rectangle r)
{ return ui_input && CheckCollisionPointRec(mpos(), r); }
static int ui_pressed(void)
{ return ui_input && IsMouseButtonPressed(MOUSE_BUTTON_LEFT); }

static int button(Rectangle r, const char *label, int active)
{
    int hot = ui_hit(r);
    Color bg = active ? BTN_ON : hot ? BTN_HOT : BTN_IDLE;
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, BTN_EDGE);
    int fs = 24, tw = ui_measure(label, fs);
    while (tw > r.width - 6 && fs > 10) { fs -= 2; tw = ui_measure(label, fs); }
    ui_text(label, r.x + (r.width - tw) / 2, r.y + (r.height - fs) / 2, fs,
            RAYWHITE);
    return hot && ui_pressed();
}

/* ---- guest reads (big-endian, through the host's memory) ---- */
static uint16_t r16(uint32_t a) { return (uint16_t)m68k_read_memory_16(a); }
__attribute__((unused)) static int16_t s16(uint32_t a) { return (int16_t)r16(a); }
static uint32_t r32(uint32_t a) { return m68k_read_memory_32(a); }

/* label/value row: label yellow at x, value RAYWHITE at x+dx */
static void kv(int x, int y, const char *key, const char *value, int dx)
{
    ui_text(key, x, y, 20, LABEL);
    ui_text(value, x + dx, y, 20, RAYWHITE);
}
static void page_head(const char *title)
{
    ui_text(title, 16, 12, 34, RAYWHITE);
}

/* Edge-detect a joystick button, ignoring whatever it reads on the very
 * FIRST look.  A pad holding a button at startup -- or a device that
 * reports a phantom button down, which this family of dongles does --
 * would otherwise register as a press on frame one and keep flipping
 * pages.  That is what made the debug buttons look dead: the page moved
 * before anyone touched anything. */
static int js_edge(int pad, int button)
{
    static uint8_t was[2][16], seen[2][16];
    if (pad < 0 || pad > 1 || button < 0 || button > 15) return 0;
    int down = js_present(pad) && js_button_down(pad, button);
    int edge = down && !was[pad][button] && seen[pad][button];
    was[pad][button] = (uint8_t)down;
    seen[pad][button] = 1;
    return edge;
}

/* ---- course geometry ------------------------------------------------ */
/* The race snapshot the track player holds, when it holds the course the
 * page is showing.  Declared here so the 2D views read the SAME table the
 * road chain is drawing from -- otherwise the map and the road view could
 * be two different courses. */
static const uint8_t *course_snapshot_table(void);

static uint8_t course_byte(int seg, int off)
{
    const uint8_t *snap = course_snapshot_table();
    if (snap) return snap[seg * COURSE_RECORD + off];
    if (course_file)
        return course_file[COURSE_FILE_TABLE + seg * COURSE_RECORD + off];
    return (uint8_t)m68k_read_memory_8(COURSE_BASE +
                                       (uint32_t)seg * COURSE_RECORD + off);
}
static int8_t course_curve(int seg) { return (int8_t)course_byte(seg, 5); }
/* Byte 6 is what $213edc feeds into its vertical accumulator, but that is
 * a PER-LINE delta for drawing the road, and it alternates sign every few
 * segments -- integrating it as terrain gives a washboard.  The course's
 * actual gradient channel is byte 2: it is ease-ramped like the curvature
 * (0,1,2,3,4,4,...,4,3,2,1,0) and integrates into 18 long hills across the
 * course, which is the smooth long climbs and descents you drive. */
static int8_t course_slope(int seg) { return (int8_t)course_byte(seg, 2); }
/* The course names are drawn as glyph bitmaps, not stored as text, so a
 * course cannot be identified by reading a string.  Hash the curvature and
 * slope bytes of the whole table instead: it is stable, and each course
 * gets its own value.  $0ff4d6d5 is the course reached by the objwalk
 * route (title -> "FOREST COURSE" -> race), which is how it is named. */
static uint32_t course_hash(void)
{
    if (course_file) return course_file_id;
    uint32_t h = 2166136261u;
    for (int i = 0; i < COURSE_SEGMENTS; i++) {
        h ^= (uint8_t)course_curve(i); h *= 16777619u;
        h ^= (uint8_t)course_slope(i); h *= 16777619u;
    }
    return h;
}
static const char *course_name(void)
{
    /* The snapshot decides which course is on show, so it names it too;
     * the .l2c's embedded name only applies when there is no snapshot. */
    if (course_snapshot_table()) return COURSES[course_sel].name;
    if (course_file && course_file_name[0]) return course_file_name;
    switch (course_hash()) {
    case 0x0ff4d6d5u: return "FOREST";
    default: return NULL;
    }
}

/* Load a course file produced by tools/course_extract.py. */
static int course_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < COURSE_FILE_TABLE + COURSE_SEGMENTS * COURSE_RECORD) {
        fclose(f);
        return 0;
    }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    if (memcmp(buf, "L2C1", 4)) { free(buf); return 0; }
    free(course_file);
    course_file = buf;
    course_file_id = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8)
                   | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
    memcpy(course_file_name, buf + 12, 31);
    course_file_name[31] = 0;
    const uint8_t *c = buf + 44;
    course_file_sky   = (Color){c[0], c[1], c[2], 255};
    course_file_grass = (Color){c[3], c[4], c[5], 255};
    course_file_road  = (Color){c[6], c[7], c[8], 255};
    fprintf(stderr, "lotus2_view: loaded %s (%s, id $%08x)\n",
            path, course_file_name[0] ? course_file_name : "unnamed",
            course_file_id);
    return 1;
}

static int course_data_ready(void)
{
    for (int i = 0; i < 240; i++)
        if (course_curve(i)) return 1;
    return 0;
}
__attribute__((unused)) static int course_here(void)
{ return (int)((r32(A3 + 0x30d8) >> 16) & 0xffff); }

/* Course preview: a 3D drive-along built from the same curvature/slope
 * table the game reads, plus a scrollable top-down map.  Scrubbing is
 * independent of the car (FOLLOW re-locks to it), so the whole course can
 * be inspected without driving there -- a wrong stride or index shows up
 * as a scribbled map or a road that tears apart in 3D.
 */
static float course_scrub = 0;   /* segment index, fractional */
static int course_follow = 1;
static float course_zoom = 1.0f;
static int /* keeps driving */;      /* auto-drive the preview */
static int course_drive = 1;        /* poke the game to the scrub position */
static Texture2D game_screen;       /* live game frame, drawn in the 3D pane */
/* One sixteenth of a segment per frame, which is the smoothest the
 * renderer can go: $213edc takes the top FOUR BITS of the course
 * position's fraction, so there are exactly sixteen distinct views
 * between one course record and the next.  A frame per sub-step means
 * the picture changes every frame and the road flows.
 *
 * It is deliberately not the rate the game itself travels at.  Measured
 * from race snapshots 100 frames apart, the car covers about 0.0135
 * segments a frame -- five frames per sub-step, which reads as a still
 * picture that twitches rather than as driving.  This is a preview, and
 * it is better for it to move. */
#define SEG_PER_FRAME (1.0f / 16.0f)
static float course_speed = SEG_PER_FRAME;

/* Integrate the course into a centreline + height once per frame. */
typedef struct { Vector2 p; float h; } CourseNode;
/* Heights smoothed for the 3D preview.  The slope byte is one step per
 * segment, so the raw double integral has a kink at every segment join and
 * the road reads as a washboard.  A few 1-2-1 passes keep the crests where
 * they are and take the facets out. */
static float course_h_smooth[COURSE_SEGMENTS + 1];
static CourseNode course_path[COURSE_SEGMENTS + 1];
static float course_minx, course_maxx, course_miny, course_maxy;
static int course_turns_l, course_turns_r, course_hills;

static void course_integrate(void)
{
    float x = 0, y = 0, h = 0, heading = 0;
    course_minx = course_maxx = course_miny = course_maxy = 0;
    course_turns_l = course_turns_r = course_hills = 0;
    course_path[0] = (CourseNode){{0, 0}, 0};
    for (int i = 0; i < COURSE_SEGMENTS; i++) {
        int c = course_curve(i), sl = course_slope(i);
        if (c > 0) course_turns_r++;
        else if (c < 0) course_turns_l++;
        if (sl) course_hills++;
        heading += c * 0.012f;
        /* The slope byte is a HEIGHT step, not a pitch step: integrating it
         * twice turns every sustained gradient into a parabola, which is
         * why a steady climb used to read as an endless rise and fall. */
        h += sl * 0.05f;
        x += sinf(heading);
        y += cosf(heading);
        course_path[i + 1] = (CourseNode){{x, y}, h};
        if (x < course_minx) course_minx = x;
        if (x > course_maxx) course_maxx = x;
        if (y < course_miny) course_miny = y;
        if (y > course_maxy) course_maxy = y;
    }
    for (int i = 0; i <= COURSE_SEGMENTS; i++)
        course_h_smooth[i] = course_path[i].h;
    for (int pass = 0; pass < 8; pass++) {
        float prev = course_h_smooth[0];
        for (int i = 1; i < COURSE_SEGMENTS; i++) {
            float cur = course_h_smooth[i];
            course_h_smooth[i] =
                (prev + 2.0f * cur + course_h_smooth[i + 1]) * 0.25f;
            prev = cur;
        }
    }
}

/* ---- road-only capture ----------------------------------------------
 * The cars and HUD are drawn into the same bitplanes as the road, later
 * in the frame.  So to see the track exactly as the game draws it but
 * with nothing on it, snapshot the draw buffer partway through the frame
 * -- after the road pipeline at $212f12..$212f32 has laid the road down,
 * before the object passes that follow it.  A PC hook does the grab and
 * decodes the planes with the live copper palette, so what you see is
 * the game's own output, not a reconstruction.
 */
static uint32_t road_capture_pc = 0x212f36;
static uint32_t road_img[VIEW_W * VIEW_H];
static Texture2D road_tex;
static int road_valid;
static float course_rendered_at = -1;

/* Race screen layout, read out of the gameplay copper list rather than
 * assumed: 4 bitplanes based at $2f8a(A3)+2, plane stride $20d0, row
 * stride 42 bytes (40 fetched + modulo 2).  $20d0 is the same constant
 * road_blitqueue() carries in D7, and 42 is the `adda.w #$2a,A0` the band
 * blitter steps its destination by -- so this is the game's own bitmap
 * geometry, not a guess. */
/* The HUD (speed bar, position, score, countdown) occupies the top 34
 * rows of the race screen. */
#define ROAD_TOP 34
/* the position and lap digits hang below the HUD band proper */
#define ROAD_CROP (ROAD_TOP + 24)
#define RACE_PLANE_STRIDE 0x20d0
#define RACE_ROW_STRIDE   42
#define RACE_PLANES       4

/* Decode the race bitmap out of ANY chip image, so the same proven
 * decoder serves both the live grab and the offline track player. */
static void decode_race_planes(const uint8_t *cm, uint32_t cmsize,
                               uint32_t buf, const uint16_t *pal)
{
    if (buf < 0x400 || buf >= cmsize) return;
    for (int y = 0; y < VIEW_H; y++)
        for (int x = 0; x < VIEW_W; x++) {
            unsigned idx = 0;
            for (int p = 0; p < RACE_PLANES; p++) {
                uint32_t at = buf + (uint32_t)p * RACE_PLANE_STRIDE
                            + (uint32_t)y * RACE_ROW_STRIDE
                            + (uint32_t)(x >> 3);
                if (at >= cmsize) continue;
                if ((cm[at] >> (7 - (x & 7))) & 1) idx |= 1u << p;
            }
            uint16_t c = pal[idx & 31];
            /* host packs blue high, red low (see amiga.c rgb4) */
            road_img[y * VIEW_W + x] =
                0xff000000u | (uint32_t)(((c >> 8) & 15) * 17)
                | ((uint32_t)(((c >> 4) & 15) * 17) << 8)
                | ((uint32_t)((c & 15) * 17) << 16);
        }
    road_valid = 1;
}

static void road_grab(void)
{
    uint16_t pal[32];
    amiga_get_palette(pal);
    decode_race_planes(chip, CHIP_SIZE, (r32(A3 + 0x2f8a) + 2) & ~1u, pal);
}

static void road_pc_hook(unsigned int pc)
{
    if (pc == road_capture_pc) road_grab();
}

/* Starting a course without typing its password: the page sets the
 * request, the main loop acts on it where the WHDLoad config lives. */
static int sel_request;      /* 1..8 = that course, -1 = the series */
static int sel_series;

/* Markings and weather, drawn natively over the composited frame. */
static void road_markings(Game *g, uint32_t *img);
static void weather_native(uint32_t *img, int kind, int count);
static void atmosphere_native(uint32_t *img, int course);
static void sky_despeckle(uint32_t *img);
static int road_extras = 1;

/* ---- native track player --------------------------------------------
 * The ROAD VIEW runs the GAME'S OWN road pipeline: the ported chain in
 * src/engine/road.c -- the same C the native build races on -- over a
 * real race snapshot of the selected course, captured by `make
 * course-snaps` from a replay that enters that course's password.
 *
 * Seeking uses the game's own mechanism: the course position long at
 * $30d8(A3), whose high word is the record index both $213edc and
 * $21508a walk the course table by.  So dragging the scrub bar drives
 * the real interpolator rather than a model of it, and what appears is
 * the track as the game draws it, without cars or HUD on top -- the
 * object passes that would put them there are simply not called.
 *
 * The 2D pages (map, profile, strip) still read the course table, so
 * they and this view are two readings of the same bytes.
 */
static Game rp_g;
static Blitter rp_bl;
static int rp_ready;            /* a snapshot is loaded */
/* The snapshot's own race bitmap, untouched.  Every draw starts from
 * this, because the clear below is bounded by the CURRENT horizon: over
 * a crest the horizon rises, and last frame's road above the new horizon
 * would otherwise stay on screen as a stripe of leftover hill. */
static uint8_t *rp_clean;
static uint32_t rp_clean_at;
static int rp_clean_horizon = -1;   /* $30e4 in the captured frame */
static int rp_course = -1;      /* which COURSES[] entry it holds */
static char rp_note[96];

#define RP_VIEW (A3 + 0x3054)

static const char *const ROADPLAY_NAMES[COURSE_COUNT] = {
    "forest", "night", "fog", "snow", "desert", "motorway", "marsh", "storm"
};

static void roadplay_load(int course)
{
    const char *const *NAMES = ROADPLAY_NAMES;
    if (course < 0 || course >= COURSE_COUNT) return;
    char fp[256], cp[256];
    snprintf(fp, sizeof fp, "re/pipeline/courses/%s_0_211e78_fast.bin",
             NAMES[course]);
    snprintf(cp, sizeof cp, "re/pipeline/courses/%s_0_211e78_chip.bin",
             NAMES[course]);
    size_t len = 0;
    uint8_t *nf = guest_load(fp, GUEST_FAST_SIZE, &len);
    uint8_t *nc = guest_load(cp, GUEST_CHIP_SIZE, &len);
    if (!nf || !nc) {
        free(nf); free(nc);
        rp_ready = 0;
        snprintf(rp_note, sizeof rp_note,
                 "no snapshot for %s -- run make course-snaps", NAMES[course]);
        return;
    }
    free(rp_g.fast); free(rp_g.chip);
    rp_g.fast = nf;
    rp_g.chip = nc;
    rp_g.base = rp_g.fast + (GUEST_BASE_ADDR - GUEST_FAST_ADDR);
    rp_bl = (Blitter){0};
    rp_bl.chip = rp_g.chip;
    rp_bl.chip_size = GUEST_CHIP_SIZE;
    rp_bl.bltafwm = rp_bl.bltalwm = 0xffff;
    free(rp_clean);
    rp_clean_at = (f32(&rp_g, A3 + 0x2f8e) + 2) & ~1u;
    rp_clean = malloc((size_t)RACE_PLANES * RACE_PLANE_STRIDE);
    if (rp_clean && rp_clean_at >= 0x400 &&
        rp_clean_at + (size_t)RACE_PLANES * RACE_PLANE_STRIDE
            <= GUEST_CHIP_SIZE)
        memcpy(rp_clean, rp_g.chip + rp_clean_at,
               (size_t)RACE_PLANES * RACE_PLANE_STRIDE);
    else { free(rp_clean); rp_clean = NULL; }
    rp_clean_horizon = (int16_t)f16(&rp_g, A3 + 0x30e4);
    if (rp_clean_horizon < 0) rp_clean_horizon = 0;
    if (rp_clean_horizon > VIEW_H) rp_clean_horizon = VIEW_H;
    rp_ready = 1;
    rp_course = course;
    snprintf(rp_note, sizeof rp_note,
             "the game's own road chain in native C  "
             "(the road is drawn live; the skyline is the captured "
             "frame's, moved to the horizon)");
}

/* Set while the game page draws, so its map reads the course the GAME is
 * on rather than whichever snapshot the preview page last loaded. */
static int course_read_live;

static const uint8_t *course_snapshot_table(void)
{
    if (course_read_live) return NULL;
    if (!rp_ready || rp_course != course_sel) return NULL;
    return rp_g.fast + (COURSE_BASE - GUEST_FAST_ADDR);
}

/* One frame of the road chain at an arbitrary course position. */
/* `where` is fractional on purpose.  The course position is a 16.16
 * value and the keyframe generator interpolates within a record, so
 * seeking on whole segments only ever showed 1024 discrete views and
 * every step between them was a jump. */
static void roadplay_draw(float where)
{
    if (!rp_ready) return;
    if (where < 0) where = 0;
    if (where > COURSE_SEGMENTS - 1) where = COURSE_SEGMENTS - 1;
    pf32(&rp_g, A3 + 0x30d8, (uint32_t)(where * 65536.0f));
    /* No steering.  $30dc is the steering word the perspective pass
     * picks its variant from -- zero runs the plain, centred pass, while
     * anything else leans the road the way the car was being steered.
     * The snapshot carries whatever the driver was doing at the moment
     * it was taken, which after a seek to another part of the course
     * means nothing at all: it drew the road running off the side of the
     * screen and stopping halfway down.  A preview has no driver, so the
     * camera sits square on the road. */
    pf16(&rp_g, A3 + 0x30dc, 0);

    road_sky(&rp_g);
    road_keyframes_near(&rp_g);

    /* Rebuild the picture around the horizon the generator has just
     * published.
     *
     * The sky is not painted by the copper -- clearing all four planes
     * leaves it black -- it is drawn as bands INTO the bitplanes by a
     * pass that is not ported.  All the preview has is the sky from the
     * frame the snapshot was captured on, drawn for THAT frame's
     * horizon.  Leaving it alone is what made a hill stay on screen:
     * seek somewhere with a lower horizon and the snapshot's own road,
     * which sat above its horizon, was still there.
     *
     * So the sky is SHIFTED to meet the new horizon, the way it moves in
     * the game, and everything below the horizon is cleared for the road
     * chain to draw into.
     */
    {
        uint32_t buf = (f32(&rp_g, A3 + 0x2f8e) + 2) & ~1u;
        int cur = (int16_t)f16(&rp_g, A3 + 0x30e4);
        if (cur < ROAD_CROP) cur = ROAD_CROP;
        if (cur > VIEW_H) cur = VIEW_H;
        int shift = cur - rp_clean_horizon;

        if (rp_clean && buf == rp_clean_at) {
            for (int p = 0; p < RACE_PLANES; p++) {
                uint8_t *dst = rp_g.chip + buf + (size_t)p * RACE_PLANE_STRIDE;
                const uint8_t *src = rp_clean + (size_t)p * RACE_PLANE_STRIDE;
                /* sky, aligned to the new horizon; rows that fall off
                 * the top of the captured sky repeat its topmost row */
                for (int y = ROAD_CROP; y < cur; y++) {
                    int sy = y - shift;
                    if (sy < ROAD_CROP) sy = ROAD_CROP;
                    if (sy > rp_clean_horizon - 1) sy = rp_clean_horizon - 1;
                    if (sy < 0) sy = 0;
                    memcpy(dst + (size_t)y * RACE_ROW_STRIDE,
                           src + (size_t)sy * RACE_ROW_STRIDE,
                           RACE_ROW_STRIDE);
                }
                /* the HUD band, and the ground the road is drawn onto */
                memset(dst, 0, (size_t)ROAD_CROP * RACE_ROW_STRIDE);
                memset(dst + (size_t)cur * RACE_ROW_STRIDE, 0,
                       (size_t)(VIEW_H - cur) * RACE_ROW_STRIDE);
            }
        }
    }

    road_interpolate(&rp_g, 0);
    road_band_bounds(&rp_g, RP_VIEW);
    road_perspective_near(&rp_g, f16(&rp_g, RP_VIEW + 0x98));
    road_blitqueue(&rp_g);
    road_bands(&rp_g, &rp_bl, f32(&rp_g, A3 + 0x2f8e), A3 - 0x2bd8,
               A3 - 0x4180, f16(&rp_g, A3 + 0x30e4), f16(&rp_g, A3 + 0x30ec),
               f16(&rp_g, A3 + 0x30dc), f32(&rp_g, A3 + 0x30d8), 0x2c);

    /* The weather, for the courses that have any.  The ported pass
     * ($215906 and below) does not draw -- it APPENDS records to the
     * blit queue -- so the preview builds a queue of its own, runs the
     * pass into it, and walks it through the engine's blitter.  The game
     * spreads the same blits across a frame on blitter interrupts; there
     * is nothing to spread here. */
    /* STORM only, for now.  SNOW's family ($2147xx) is ported and its
     * emitter is gated against a SNOW race, but driving the state
     * machine for it here blanks the picture -- the preview forces $2ebc
     * back to $60 every frame, and SNOW's bands are much larger ($214810
     * runs with D6 = $98 against STORM's $6e), so something it writes
     * lands where the rain's never did.  Wiring it in before that is
     * understood would trade a correct picture for a black one. */
    /* The game's own weather pass is still here, ported and gated
     * (make verify-storm), and deliberately not run: weather_native()
     * draws the rain and the snow instead.  Set this to 1 to see what
     * the 68000 does, which is the only reason to keep the switch. */
    if (0) {
        uint32_t shown = f32(&rp_g, A3 + 0x2f8a);
        pf32(&rp_g, A3 + 0x2f8a, f32(&rp_g, A3 + 0x2f8e));
        pf16(&rp_g, A3 + 0x2ebc, 0x60);       /* first stop of the machine */
        Regs wr;
        for (int i = 0; i < 8; i++) { wr.d[i] = 0; wr.a[i] = 0; }
        wr.a[3] = A3;
        wr.a[4] = 0x200004;                   /* a queue of our own */
        weather_pass(&rp_g, &wr);
        pf16(&rp_g, wr.a[4], 0);              /* the zero type ends it */
        blitq_run_records(&rp_g, &rp_bl, 0x200004);
        pf32(&rp_g, A3 + 0x2f8a, shown);
    }

    /* Show it the way the machine would.  The racing screen's colours
     * are not one palette: the sky is a copper gradient, so decoding the
     * bitplanes against a flat 32-entry table gives a black sky and a
     * blue road.  The native compositor is a per-line copper interpreter
     * -- the same one the render gate proves pixel-identical to the
     * oracle -- so run the copper list instead.
     *
     * Which list?  The game keeps one per screen buffer, so pick the one
     * whose BPL1PT points at the buffer we just drew into rather than
     * assuming an address. */
    uint32_t drawn = f32(&rp_g, A3 + 0x2f8e);
    /* FOUR copper lists, not three.  $2f60 picks which of three sky
     * regions the sky pass writes into -- $7f6d8, $7e510, $7edf4 -- and
     * each of those sits inside its own list: $7f5f0, $7e428, $7ed0c,
     * with $7fedc the fourth.  SNOW and MARSH run with $2f60 = 1, whose
     * list at $7e428 was missing here, so no list matched the buffer and
     * the view fell back to decoding against a flat palette: a purple
     * road on a black ground. */
    static const uint32_t COPLISTS[] = {0x7f5f0, 0x7ed0c, 0x7e428, 0x7fedc};
    uint32_t use = 0;
    for (int k = 0; k < 4 && !use; k++) {
        uint32_t hi = 0, lo = 0, at = COPLISTS[k];
        for (int i = 0; i < 2048; i++, at += 4) {
            uint16_t reg = g16(rp_g.chip, at), dat = g16(rp_g.chip, at + 2);
            if (reg == 0x00e0) hi = dat;
            else if (reg == 0x00e2) lo = dat;
            else if (reg == 0xffff && dat == 0xfffe) break;
        }
        if (((hi << 16) | lo) == ((drawn + 2) & ~1u)) use = COPLISTS[k];
    }
    if (use && composite(rp_g.chip, use, road_img) == 0) {
        road_valid = 1;
        /* SNOW and STORM ship with weather painted into the frame;
         * with a simulation running that would be two lots of it. */
        if (rp_course == 3 || rp_course == 7) sky_despeckle(road_img);
        road_markings(&rp_g, road_img);
        /* SNOW and STORM get weather; the rest of the courses have none
         * and inventing some would be a different game. */
        atmosphere_native(road_img, rp_course);
        weather_native(road_img, rp_course == 7 ? 1 : rp_course == 3 ? 2 : 0,
                       rp_course == 7 ? 900 : 650);
        /* LOTUS2_EDGES: print, for each drawn line, the record the road
         * chain produced beside the edges that actually came out.  The
         * mapping from `line` to a screen x is the one thing the
         * hi-res renderers still need, and it is worth measuring rather
         * than deriving -- see re/HIRES.md. */
        if (getenv("LOTUS2_EDGES")) {
            static int once;
            if (!once) {
                once = 1;
                uint16_t first = f16(&rp_g, A3 + 0x30e4);
                uint16_t limit = f16(&rp_g, A3 + 0x2eaa);
                uint32_t rec = A3 - 0x2bd8 + (uint32_t)(first * 2) * 3;
                fprintf(stderr, "row  line index  left right  centre\n");
                for (uint16_t ln = first; ln < limit && ln < VIEW_H; ln++) {
                    uint16_t v = f16(&rp_g, rec);
                    uint16_t ix = f16(&rp_g, rec + 2);
                    rec += 6;
                    /* the road is whatever differs from the verge, which
                     * is what the row's outermost pixels are */
                    uint32_t edge = road_img[ln * VIEW_W + 2];
                    int l = -1, r = -1;
                    for (int x = 0; x < VIEW_W; x++)
                        if (road_img[ln * VIEW_W + x] != edge) { l = x; break; }
                    for (int x = VIEW_W - 1; x >= 0; x--)
                        if (road_img[ln * VIEW_W + x] != edge) { r = x; break; }
                    fprintf(stderr, "%3u %5d %5u %5d %5d %7.1f\n", ln,
                            (int16_t)(0xb1 - v), ix, l, r,
                            (l >= 0 && r >= 0) ? (l + r) / 2.0 : -1.0);
                }
            }
        }
        return;
    }
    /* No matching list: fall back to the flat-palette decode. */
    uint16_t pal[32];
    for (int i = 0; i < 32; i++) pal[i] = f16(&rp_g, A3 + G_PALETTE + 2 * i);
    decode_race_planes(rp_g.chip, GUEST_CHIP_SIZE, (drawn + 2) & ~1u, pal);
}

/* ---- course preview renderer ----------------------------------------
 * Calling the game's road chain cold does not work: those routines are
 * one function's worth of shared register state ($2136f6 onward expect
 * D4-D7 and A0-A2 set by the code above them), and invoking them out of
 * context derails the CPU.  So the preview is drawn here instead -- but
 * from the REAL decoded data, not invented geometry:
 *
 *   - curvature and slope per segment come from the course table at
 *     $206183 (bytes 5 and 6), the same bytes $213edc reads;
 *   - the road's on-screen width follows the game's own edge stream,
 *     where width = x / 8 (fitted against the race frame on lines
 *     154-166 to about 1%);
 *   - sky, grass, tarmac and verge colours are sampled from the live
 *     game frame, so the preview wears the game's palette.
 *
 * It is a preview, not the shipped renderer: the pixel-exact path is
 * the native compositor, gated by make render-gate.
 */
#define PREVIEW_SEGS 200

static Color frame_pixel(int x, int y)
{
    if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H)
        return (Color){0, 0, 0, 255};
    uint32_t v = framebuf[y * SCREEN_W + x];
    return (Color){(unsigned char)(v & 0xff),
                   (unsigned char)((v >> 8) & 0xff),
                   (unsigned char)((v >> 16) & 0xff), 255};
}

static Color prev_sky, prev_grass, prev_road, prev_kerb;
static void preview_sample_colours(void)
{
    /* sky high and central (the sides carry trees), grass at the very
     * bottom corner, tarmac dead centre at the bottom of the screen */
    prev_kerb = (Color){206, 64, 64, 255};
    if (course_file) {
        prev_sky = course_file_sky;
        prev_grass = course_file_grass;
        prev_road = course_file_road;
        return;
    }
    prev_sky   = frame_pixel(GAME_OX + 160, GAME_OY + 50);
    prev_grass = frame_pixel(GAME_OX + 6,   GAME_OY + 198);
    prev_road  = frame_pixel(GAME_OX + 160, GAME_OY + 198);
    int flat = prev_road.r == prev_grass.r && prev_road.g == prev_grass.g &&
               prev_road.b == prev_grass.b;

    if (flat) {
        prev_sky   = (Color){40, 60, 170, 255};
        prev_grass = (Color){28, 92, 40, 255};
        prev_road  = (Color){56, 56, 60, 255};
    }
}

/* Roadside objects: each course record carries four (id, x) pairs in
 * bytes 7..14.  $70 and $71 always sit at x = +18 / -18, i.e. the verge
 * posts either side of the road; $82 is the common tree (1116 uses), with
 * $84-$87 the other scenery sprites and $81/$83 rare signs.  x runs about
 * +-64, so it is scaled against the road half-width. */
#define OBJ_PAIRS 4
static int obj_id(int seg, int k) { return course_byte(seg, 7 + k * 2); }
static int obj_x(int seg, int k) { return (int8_t)course_byte(seg, 8 + k * 2); }

static void put_span(int y, int x0, int x1, Color c)
{
    if (y < 0 || y >= VIEW_H) return;
    if (x0 < 0) x0 = 0;
    if (x1 > VIEW_W - 1) x1 = VIEW_W - 1;
    uint32_t v = 0xff000000u | (uint32_t)c.r | ((uint32_t)c.g << 8)
               | ((uint32_t)c.b << 16);
    for (int x = x0; x <= x1; x++) road_img[y * VIEW_W + x] = v;
}

static Color shade(Color c, float k)
{
    int r = (int)(c.r * k), g = (int)(c.g * k), b = (int)(c.b * k);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
}

/* Simple billboards: the sprites themselves live in the game's graphics
 * banks, so the preview draws readable stand-ins keyed by id class -- the
 * point is to see WHERE the scenery is and how it lines the course. */
static void draw_object(int id, float cx, float ybase, float w)
{
    float h;
    Color body, accent;
    switch (id) {
    case 0x70: case 0x71:                     /* verge post */
        h = w * 0.16f + 1.5f;
        body = (Color){232, 232, 232, 255};
        accent = (Color){206, 64, 64, 255};
        break;
    case 0x81: case 0x83:                     /* sign */
        h = w * 0.36f + 2.0f;
        body = (Color){236, 236, 236, 255};
        accent = (Color){60, 60, 66, 255};
        break;
    case 0x82:                                /* tree */
        h = w * 0.72f + 3.0f;
        body = (Color){36, 104, 44, 255};
        accent = (Color){78, 56, 34, 255};
        break;
    case 0x84: case 0x86:                     /* darker tree */
        h = w * 0.62f + 3.0f;
        body = (Color){28, 82, 40, 255};
        accent = (Color){70, 50, 32, 255};
        break;
    case 0x85: case 0x87:                     /* bush / rock */
        h = w * 0.34f + 2.0f;
        body = (Color){52, 96, 52, 255};
        accent = body;
        break;
    default:
        return;                               /* unidentified id: draw nothing */
    }
    if (h < 1.5f) h = 1.5f;
    float half = h * 0.30f + 0.6f;
    int y1 = (int)ybase, y0 = (int)(ybase - h);
    if (y0 < 0) y0 = 0;
    if (y1 > VIEW_H - 1) y1 = VIEW_H - 1;

    if (id == 0x70 || id == 0x71 || id == 0x81 || id == 0x83) {
        /* post, with a board on top for the signs */
        int pw2 = (int)(half * 0.35f);
        if (pw2 < 1) pw2 = 1;
        for (int y = y0; y <= y1; y++)
            put_span(y, (int)cx - pw2, (int)cx + pw2, accent);
        int board = (id == 0x81 || id == 0x83) ? (int)(h * 0.45f)
                                               : (int)(h * 0.30f);
        for (int y = y0; y <= y0 + board && y <= y1; y++)
            put_span(y, (int)(cx - half), (int)(cx + half), body);
    } else {
        /* canopy: a triangle that narrows toward the top */
        int trunk = (int)(h * 0.25f);
        for (int y = y1 - trunk; y <= y1; y++)
            put_span(y, (int)cx - 1, (int)cx + 1, accent);
        int span = y1 - trunk - y0;
        if (span < 1) span = 1;
        for (int y = y0; y <= y1 - trunk; y++) {
            float t = (float)(y - y0) / span;
            float ww = half * (0.25f + 0.75f * t);
            put_span(y, (int)(cx - ww), (int)(cx + ww), body);
        }
    }
}

static void render_road_frame(int segment)
{
    preview_sample_colours();
    const int HORIZON = 96;
    /* sky and ground */
    for (int y = 0; y < VIEW_H; y++)
        put_span(y, 0, VIEW_W - 1, y < HORIZON ? prev_sky : prev_grass);

    /* Project every segment boundary ahead of the camera.
     *
     * Height is taken from the globally integrated profile and then
     * PITCH-STABILISED against the camera: subtract the camera's own
     * height and the linear term of the gradient it is currently sitting
     * on.  Without that, a constant gradient integrates away from zero and
     * the road reads as an endless hill that rises and falls; with it, a
     * steady slope looks like a steady slope and only CHANGES in gradient
     * become crests and dips, which is what you see from the car.
     */
    static float py[PREVIEW_SEGS + 1], pcx[PREVIEW_SEGS + 1],
                 pw[PREVIEW_SEGS + 1];
    int cam = segment;
    float cam_h = course_h_smooth[cam];
    float cam_grad = (cam + 1 <= COURSE_SEGMENTS)
                   ? course_h_smooth[cam + 1] - cam_h : 0.0f;
    float dx = 0, ddx = 0;
    int last = -1;
    for (int i = 0; i <= PREVIEW_SEGS; i++) {
        int seg = segment + i;
        if (seg > COURSE_SEGMENTS) break;
        /* relative, pitch-stabilised height with a light smoothing pass */
        float h = course_h_smooth[seg] - cam_h - cam_grad * (float)i;
        float z = i + 0.9f;
        float scale = 190.0f / z;
        py[i] = HORIZON + (1.45f - h * 0.55f) * scale * 0.62f;
        pcx[i] = VIEW_W * 0.5f - dx * scale * 0.030f;
        pw[i] = 2.05f * scale * 0.5f;     /* matches width = x/8 in-game */
        last = i;
        if (seg < COURSE_SEGMENTS) {
            float cu0 = (float)course_curve(seg);
            float cu1 = (seg + 1 < COURSE_SEGMENTS)
                      ? (float)course_curve(seg + 1) : cu0;
            const int SUB = 4;
            for (int k = 0; k < SUB; k++) {
                float u = (k + 0.5f) / SUB;
                float e = u * u * (3.0f - 2.0f * u);   /* smoothstep */
                ddx += (cu0 + (cu1 - cu0) * e) * (0.055f / SUB);
                dx  += ddx / SUB;
            }
        }
    }

    /* paint far to near so near geometry covers far */
    for (int i = last - 1; i >= 0; i--) {
        float ny = py[i], fy = py[i + 1];
        if (ny <= fy) continue;                    /* over a crest */
        int y0 = (int)(fy < 0 ? 0 : fy), y1 = (int)ny;
        if (y1 > VIEW_H - 1) y1 = VIEW_H - 1;
        int seg = segment + i;
        int band = (seg / 3) & 1;
        Color tar   = shade(prev_road,  band ? 1.10f : 0.92f);
        Color grass = shade(prev_grass, band ? 1.14f : 0.90f);
        Color kerb  = band ? prev_kerb : (Color){228, 228, 228, 255};
        for (int y = y0; y <= y1; y++) {
            float t = (ny - fy) > 0.01f ? (ny - y) / (ny - fy) : 0;
            float cx = pcx[i] + (pcx[i + 1] - pcx[i]) * t;
            float w  = pw[i] + (pw[i + 1] - pw[i]) * t;
            float k = w * 0.13f + 1.0f;
            put_span(y, 0, VIEW_W - 1, grass);
            put_span(y, (int)(cx - w - k), (int)(cx + w + k), kerb);
            put_span(y, (int)(cx - w), (int)(cx + w), tar);
            if (((seg / 3) & 1) && w > 2.5f)
                put_span(y, (int)(cx - w * 0.03f - 0.5f),
                         (int)(cx + w * 0.03f + 0.5f),
                         (Color){236, 236, 236, 255});
        }
    }
    /* roadside objects, far to near so near ones overlap */
    for (int i = last - 1; i >= 0; i--) {
        int seg = segment + i;
        if (seg >= COURSE_SEGMENTS) continue;
        float ybase = py[i];
        if (ybase < HORIZON - 4 || ybase > VIEW_H + 40) continue;
        float w = pw[i];
        if (w < 0.8f) continue;
        /* Very near segments project to an enormous half-width, and an
         * object scaled off that fills the screen -- which is why the
         * closest trees were covering the road.  Cap the size used for
         * scenery and skip anything that has come past the camera. */
        float ow = w > 26.0f ? 26.0f : w;
        for (int k = 0; k < OBJ_PAIRS; k++) {
            int id = obj_id(seg, k);
            if (!id) continue;
            float ox = obj_x(seg, k) / 18.0f;          /* $70/$71 = one road half */
            float cx = pcx[i] + ox * w;
            if (cx < -40 || cx > VIEW_W + 40) continue;
            draw_object(id, cx, ybase, ow);
        }
    }

    /* roadside objects, far to near so near ones overlap */
    for (int i = last - 1; i >= 0; i--) {
        int seg = segment + i;
        if (seg >= COURSE_SEGMENTS) continue;
        float ybase = py[i];
        if (ybase < HORIZON - 4 || ybase > VIEW_H + 40) continue;
        float w = pw[i];
        if (w < 0.8f) continue;
        /* Very near segments project to an enormous half-width, and an
         * object scaled off that fills the screen -- which is why the
         * closest trees were covering the road.  Cap the size used for
         * scenery and skip anything that has come past the camera. */
        float ow = w > 26.0f ? 26.0f : w;
        for (int k = 0; k < OBJ_PAIRS; k++) {
            int id = obj_id(seg, k);
            if (!id) continue;
            float ox = obj_x(seg, k) / 18.0f;          /* $70/$71 = one road half */
            float cx = pcx[i] + ox * w;
            if (cx < -40 || cx > VIEW_W + 40) continue;
            draw_object(id, cx, ybase, ow);
        }
    }

    road_valid = 1;
}

/* ---- the course intro ------------------------------------------------
 * The screen is a framed photograph on black, and the game zooms it up
 * as a transition -- which is where the picture-in-a-picture comes from:
 * for the length of the zoom there is a big copy behind the small one.
 *
 * Rather than try to remove something that is genuinely being drawn,
 * this finds the photograph and shows THAT, filling the window.  The
 * frame, the black surround and the doubling all go, because none of
 * them are being displayed any more -- the picture is.
 *
 * Detected from the picture rather than from game state: a screen that
 * is mostly black with one wide block of colour in the middle is this
 * screen and nothing else in the game looks like it.
 */
static int intro_photo(Rectangle *src)
{
    int first = -1, last = -1, dark = 0;
    for (int y = 0; y < GAME_H; y++) {
        int lit = 0;
        for (int x = 0; x < GAME_W; x++)
            if ((framebuf[(GAME_OY + y) * SCREEN_W + GAME_OX + x] &
                 0x00ffffffu)) lit++;
        if (lit < 8) dark++;
        if (lit > 120) { if (first < 0) first = y; last = y; }
    }
    if (first < 0 || last - first < 50) return 0;
    if (dark < GAME_H / 3) return 0;          /* a race frame is not black */

    int l = -1, r = -1;
    for (int x = 0; x < GAME_W; x++) {
        int lit = 0;
        for (int y = first; y <= last; y++)
            if ((framebuf[(GAME_OY + y) * SCREEN_W + GAME_OX + x] &
                 0x00ffffffu)) lit++;
        if (lit > (last - first) / 3) { if (l < 0) l = x; r = x; }
    }
    if (l < 0 || r - l < 80) return 0;
    /* inside the frame, not on it */
    *src = (Rectangle){GAME_OX + l + 2, GAME_OY + first + 2,
                       (float)(r - l - 3), (float)(last - first - 3)};
    return 1;
}

/* ---- car colour ------------------------------------------------------
 * The player's Esprit is drawn with two palette entries: $a00 for the
 * body and $600 for its shaded side.  Sampling the indices under the car
 * in a race frame says so -- 7 and 3, about 1100 pixels between them --
 * and the palette confirms it.
 *
 * Recolouring is done on the finished picture rather than in the
 * palette, for one reason: those two entries are used by the HUD as
 * well, and repainting the speed bar because you wanted a green car
 * would be a bug, not a feature.  Below the HUD band, the only red of
 * those two shades on screen is the car.
 *
 * The opponents are a separate matter and the note at the bottom says
 * what can and cannot be done about them.
 */
typedef struct { const char *name; uint8_t br, bg, bb, dr, dg, db; } CarHue;
static const CarHue CAR_HUES[] = {
    { "RED",    0xaa, 0x00, 0x00,  0x66, 0x00, 0x00 },   /* as shipped */
    { "BLUE",   0x22, 0x44, 0xdd,  0x11, 0x22, 0x77 },
    { "GREEN",  0x22, 0xaa, 0x33,  0x11, 0x55, 0x22 },
    { "YELLOW", 0xee, 0xcc, 0x11,  0x99, 0x77, 0x00 },
    { "ORANGE", 0xee, 0x77, 0x00,  0x88, 0x44, 0x00 },
    { "PURPLE", 0x99, 0x33, 0xcc,  0x55, 0x11, 0x77 },
    { "WHITE",  0xdd, 0xdd, 0xdd,  0x88, 0x88, 0x88 },
    { "BLACK",  0x33, 0x33, 0x33,  0x11, 0x11, 0x11 },
};
#define CAR_HUE_COUNT ((int)(sizeof CAR_HUES / sizeof CAR_HUES[0]))
static int car_hue;                 /* 0 = leave it alone */
const char *car_hue_name(void) { return CAR_HUES[car_hue].name; }

/* The two reds, in the host's pixel order (blue high, red low). */
#define CAR_BRIGHT 0xff0000aau
#define CAR_DARK   0xff000066u

static void car_recolour(uint32_t *img, int w, int h, int from_y)
{
    if (!car_hue) return;
    const CarHue *c = &CAR_HUES[car_hue];
    uint32_t br = 0xff000000u | c->br | ((uint32_t)c->bg << 8)
                | ((uint32_t)c->bb << 16);
    uint32_t dk = 0xff000000u | c->dr | ((uint32_t)c->dg << 8)
                | ((uint32_t)c->db << 16);
    for (int y = from_y; y < h; y++) {
        uint32_t *row = img + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            if (row[x] == CAR_BRIGHT) row[x] = br;
            else if (row[x] == CAR_DARK) row[x] = dk;
        }
    }
}

/* ---- markings and weather, drawn natively -----------------------------
 * The road's geometry is known exactly, MEASURED rather than derived:
 * printing each line's record beside the edges the 1x render produced
 * gives `left + line == 335` on every row, so the left edge is at
 * 335 - line and the width is twice the strip index.  Both edges are
 * numbers before anything is drawn, which is what makes it possible to
 * put things on the road properly.
 *
 *     row  line index  left right     left + line
 *     145   118    18   217   252         335
 *     168   291   118    44   276         335
 *
 * Distance comes from the same place.  Road width on screen falls as
 * 1/z, so z is proportional to 1/index -- no separate depth buffer is
 * needed and none is kept.
 */

typedef struct { float l, r; int have; } RoadEdge;
static RoadEdge road_edge[VIEW_H];
static uint16_t road_first, road_limit;

/* Read this frame's edges out of the records the road chain just left. */
static void road_read_edges(Game *g)
{
    road_first = f16(g, A3 + 0x30e4);
    road_limit = f16(g, A3 + 0x2eaa);
    if (road_limit > VIEW_H) road_limit = VIEW_H;
    for (int i = 0; i < VIEW_H; i++) road_edge[i].have = 0;
    uint32_t rec = A3 - 0x2bd8 + (uint32_t)(road_first * 2) * 3;
    for (uint16_t ln = road_first; ln < road_limit; ln++) {
        int16_t line = (int16_t)(0xb1 - f16(g, rec));
        uint16_t index = f16(g, rec + 2);
        rec += 6;
        if (!index) continue;
        road_edge[ln].l = 335.0f - line;
        road_edge[ln].r = road_edge[ln].l + 2.0f * index;
        road_edge[ln].have = index;
    }
}

static void px(uint32_t *img, int x, int y, uint32_t c)
{
    if (x >= 0 && x < VIEW_W && y >= 0 && y < VIEW_H)
        img[y * VIEW_W + x] = c;
}

/* Centre dashes and edge lines, on every course.
 *
 * The game only draws markings where its strip art has them, which is
 * why some roads have none.  These are drawn from the geometry instead,
 * so they are there on all eight, and they scroll with the car because
 * the phase comes from the course position rather than from a counter.
 */
void road_markings(Game *g, uint32_t *img)
{
    if (!road_extras) return;
    road_read_edges(g);
    const uint32_t white = 0xffe8e8e8u;
    float travel = f32(g, A3 + 0x30d8) / 65536.0f;

    for (uint16_t y = road_first; y < road_limit; y++) {
        if (!road_edge[y].have) continue;
        float l = road_edge[y].l, r = road_edge[y].r;
        float w = r - l;
        if (w < 6) continue;                  /* too far to resolve */

        /* z from the width: the road is 1/z wide, so z is 1/width */
        float z = 240.0f / w;
        /* dashes every few units of z, marching with the car */
        float phase = z * 1.6f - travel * 3.0f;
        int on = ((int)floorf(phase) & 1) == 0;

        int cx = (int)((l + r) * 0.5f);
        int half = (int)(w * 0.02f);
        if (half < 0) half = 0;
        if (on)
            for (int x = cx - half; x <= cx + half; x++) px(img, x, y, white);

        /* a solid line just inside each edge, so the road reads at any
         * width -- the dashes alone vanish into the horizon */
        int inset = (int)(w * 0.06f);
        if (inset < 1) inset = 1;
        for (int k = 0; k <= (w > 40 ? 1 : 0); k++) {
            px(img, (int)(l + inset) + k, y, white);
            px(img, (int)(r - inset) - k, y, white);
        }
    }
}

/* ---- weather ---------------------------------------------------------
 * The game builds its rain and snow every frame out of blit records --
 * four bands of shifted shapes queued and drawn by the blitter -- which
 * is how you do it with a 7 MHz 68000 and no floating point.  Natively
 * there is no reason to: particles can carry a real position and a real
 * velocity, fall at a rate that does not depend on the frame, and be
 * placed to the fraction of a pixel.
 *
 * They still take their colour from the game's own palette, sampled out
 * of the rendered frame, so a storm looks like Lotus 2's storm rather
 * than like weather from somewhere else.
 */
#define WX_MAX 700
typedef struct { float x, y, vx, vy, len; } Drop;
static Drop wx_drop[WX_MAX];
static int wx_count, wx_kind;      /* 1 = rain, 2 = snow */
static unsigned wx_seed = 1;

static float wx_rand(void)
{
    wx_seed = wx_seed * 1103515245u + 12345u;
    return (float)((wx_seed >> 8) & 0xffff) / 65536.0f;
}

static void wx_spawn(Drop *d, int kind, int anywhere)
{
    d->x = wx_rand() * VIEW_W;
    d->y = anywhere ? wx_rand() * VIEW_H : -wx_rand() * 20.0f;
    if (kind == 1) {                       /* rain: fast, slanted, long */
        d->vy = 5.5f + wx_rand() * 3.0f;
        d->vx = -1.1f - wx_rand() * 0.6f;
        d->len = 3.0f + wx_rand() * 3.0f;
    } else {                               /* snow: slow, drifting, round */
        d->vy = 0.7f + wx_rand() * 0.8f;
        d->vx = (wx_rand() - 0.5f) * 0.9f;
        d->len = 1.0f;
    }
}

/* `kind` 0 clears the weather, 1 rain, 2 snow. */
void weather_native(uint32_t *img, int kind, int count)
{
    if (!road_extras) kind = 0;
    if (kind != wx_kind || count != wx_count) {
        wx_kind = kind;
        wx_count = count > WX_MAX ? WX_MAX : count;
        for (int i = 0; i < wx_count; i++) wx_spawn(&wx_drop[i], kind, 1);
    }
    if (!kind) return;

    /* colour from the picture: the brightest thing in the sky for snow,
     * and for rain a blue lifted out of the palette the frame is using */
    uint32_t tint = kind == 1 ? 0xffd08040u : 0xfff0f0f0u;

    for (int i = 0; i < wx_count; i++) {
        Drop *d = &wx_drop[i];
        d->x += d->vx;
        d->y += d->vy;
        if (kind == 2) d->x += sinf((d->y + i) * 0.08f) * 0.35f;
        if (d->y > VIEW_H || d->x < -8 || d->x > VIEW_W + 8)
            wx_spawn(d, kind, 0);
        int x = (int)d->x, y0 = (int)d->y;
        if (kind == 1) {
            for (float t = 0; t < d->len; t += 1.0f)
                px(img, (int)(d->x - d->vx * t * 0.35f),
                   (int)(d->y - d->vy * t * 0.35f), tint);
        } else {
            px(img, x, y0, tint);
        }
    }
}


/* Take the game's BAKED weather out of the picture.
 *
 * The captured frame for SNOW and STORM has the 68000's own snow and
 * rain drawn into it, and now that the weather is simulated that would
 * be two lots of it -- one falling, one nailed to the backdrop.  The
 * baked kind is one pixel wide and its neighbours agree with each other,
 * which is exactly what a despeckle removes; clouds, hills, trees and
 * the road are all wider than that and survive.
 *
 * Horizontal first, then vertical, because rain is drawn as one-pixel
 * vertical streaks and snow as single dots.
 */
static void sky_despeckle(uint32_t *img)
{
    static uint32_t tmp[VIEW_W * VIEW_H];
    memcpy(tmp, img, sizeof tmp);
    for (int y = 0; y < VIEW_H; y++)
        for (int x = 1; x < VIEW_W - 1; x++) {
            uint32_t l = tmp[y * VIEW_W + x - 1], c = tmp[y * VIEW_W + x];
            uint32_t r = tmp[y * VIEW_W + x + 1];
            if (c != l && l == r) img[y * VIEW_W + x] = l;
        }
    memcpy(tmp, img, sizeof tmp);
    for (int y = 1; y < VIEW_H - 1; y++)
        for (int x = 0; x < VIEW_W; x++) {
            uint32_t u = tmp[(y - 1) * VIEW_W + x], c = tmp[y * VIEW_W + x];
            uint32_t d = tmp[(y + 1) * VIEW_W + x];
            if (c != u && u == d) img[y * VIEW_W + x] = u;
        }
}

/* ---- atmosphere ------------------------------------------------------
 * Fog and night are the two the original gets least out of.  Fog is a
 * flat grey band and night is a dark palette; neither has any DEPTH to
 * it, because depth costs cycles a 68000 does not have spare.
 *
 * We already know the depth of every row for nothing: the road is 1/z
 * wide, so z falls straight out of the width the road chain published.
 * Everything below is that one number spent well.
 *
 * All of it stays inside the frame's own palette range -- these blend
 * and scale what the game drew rather than painting over it, so a fogged
 * FOREST still looks like Lotus 2 and not like a grey rectangle.
 */
static float atm_depth[VIEW_H];      /* 0 at the camera, 1 at the horizon */
static int atm_have[VIEW_H];

static void atm_read_depth(void)
{
    for (int i = 0; i < VIEW_H; i++) atm_have[i] = 0;
    float wmax = 1.0f;
    for (uint16_t y = road_first; y < road_limit; y++)
        if (road_edge[y].have) {
            float w = road_edge[y].r - road_edge[y].l;
            if (w > wmax) wmax = w;
        }
    for (uint16_t y = road_first; y < road_limit; y++) {
        if (!road_edge[y].have) continue;
        float w = road_edge[y].r - road_edge[y].l;
        if (w < 1) w = 1;
        /* z ~ 1/w, normalised so the widest row on screen is z = 0 */
        float z = (wmax / w - 1.0f) / 12.0f;
        if (z < 0) z = 0;
        if (z > 1) z = 1;
        atm_depth[y] = z;
        atm_have[y] = 1;
    }
}

static void unpack(uint32_t v, int *r, int *g, int *b)
{   /* the host packs blue high, red low */
    *r = v & 0xff; *g = (v >> 8) & 0xff; *b = (v >> 16) & 0xff;
}
static uint32_t pack(int r, int g, int b)
{
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return 0xff000000u | (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}

/* A flash that fires now and then, for the storm.  Returns 0..1. */
static float atm_lightning(void)
{
    static int until, next = 40;
    static float level;
    if (--next <= 0) {
        /* a strike every two to five seconds at 50 Hz, lasting two to
         * four frames, sometimes a double */
        next = 100 + (int)(wx_rand() * 180.0f);
        until = 2 + (int)(wx_rand() * 3.0f);
        level = 0.55f + wx_rand() * 0.45f;
    }
    if (until > 0) {
        until--;
        if (getenv("LOTUS2_WX")) fprintf(stderr, "lightning %.2f\n", level);
        return level;
    }
    return 0.0f;
}

/* `course`: 1 NIGHT, 2 FOG, 7 STORM; anything else is left alone. */
void atmosphere_native(uint32_t *img, int course)
{
    if (!road_extras) return;
    atm_read_depth();

    float flash = (course == 7) ? atm_lightning() : 0.0f;

    for (int y = 0; y < VIEW_H; y++) {
        /* above the road the depth is the horizon's */
        float z = atm_have[y] ? atm_depth[y] : 1.0f;
        uint32_t *row = img + y * VIEW_W;

        for (int x = 0; x < VIEW_W; x++) {
            int r, g, b;
            unpack(row[x], &r, &g, &b);

            if (course == 2) {
                /* FOG: thickens with distance, and a little with height,
                 * so it sits in the valley rather than hanging flat */
                float t = z * 0.92f;
                if (t > 0.92f) t = 0.92f;
                const int FR = 196, FG = 200, FB = 206;
                r += (int)((FR - r) * t);
                g += (int)((FG - g) * t);
                b += (int)((FB - b) * t);
            } else if (course == 1) {
                /* NIGHT: everything falls away with distance, and the
                 * headlights put a wedge of light on the road ahead --
                 * which the original has no notion of at all */
                float dark = 1.0f - z * 0.72f;
                float lit = 0.0f;
                if (atm_have[y]) {
                    float c = (road_edge[y].l + road_edge[y].r) * 0.5f;
                    float half = (road_edge[y].r - road_edge[y].l) * 0.5f;
                    float d = fabsf(x - c) / (half * 2.4f + 1.0f);
                    if (d < 1.0f) lit = (1.0f - d) * (1.0f - z) * 0.85f;
                }
                float k = dark + lit;
                r = (int)(r * k + lit * 40);
                g = (int)(g * k + lit * 38);
                b = (int)(b * k + lit * 26);
            }

            if (flash > 0.0f) {
                float k = 1.0f + flash * (0.9f - z * 0.35f);
                r = (int)(r * k + flash * 34);
                g = (int)(g * k + flash * 34);
                b = (int)(b * k + flash * 46);
            }
            row[x] = pack(r, g, b);
        }
    }
}

/* ---- 3D view: the GAME's own road renderer ---------------------------
 * The preview is not a debug approximation -- it is the game's frame.
 * With DRIVE on, the viewer writes the scrub position into the course
 * position long at $30d8(A3) before each emulated frame, so the game's
 * own road pipeline ($212f12 chain: sky bands, keyframe generator, edge
 * interpolator, blit queue) renders that part of the course.  Scrubbing
 * therefore shows exactly what the game shows when you drive there.
 *
 * The racing screen sits at raster offset (17,18) in the host's 352x288
 * frame -- measured by the render gate, which proves that window is
 * pixel-identical to the native compositor's output.
 */


static void course_draw_3d(Rectangle r)
{
    DrawRectangleRec(r, (Color){10, 10, 14, 255});

    /* The caption gets its own strip.  Drawn over the picture it sat on
     * top of the game's own HUD and neither could be read. */
    const int CAP = 30;
    char t[160];
    snprintf(t, sizeof t, "ROAD VIEW  %s",
             rp_ready ? rp_note : "preview drawn from the decoded track table");
    ui_text(t, (int)r.x + 8, (int)r.y + 5, 20, LABEL);

    /* Show the road, not the space where the HUD was.  The HUD band is
     * blanked before the chain runs, and leaving it in put a black bar a
     * third of the panel deep above the picture, which reads as the road
     * sitting low and off centre. */
    Rectangle inner = {r.x, r.y + CAP, r.width, r.height - CAP};
    int road_h = GAME_H - ROAD_CROP;
    float sc = inner.width / (float)GAME_W;
    float sy = inner.height / (float)road_h;
    if (sy < sc) sc = sy;
    Rectangle dst = {inner.x + (inner.width - GAME_W * sc) * 0.5f,
                     inner.y + (inner.height - road_h * sc) * 0.5f,
                     GAME_W * sc, road_h * sc};
    UpdateTexture(road_tex, road_img);
    DrawTexturePro(road_tex,
                   (Rectangle){0, ROAD_CROP, GAME_W, GAME_H - ROAD_CROP},
                   dst, (Vector2){0, 0}, 0.0f, WHITE);
    DrawRectangleLinesEx(r, 1, GRID);
}

/* ---- scrollable top-down map ---------------------------------------- */
/* ---- 2D top-down: the curves ----------------------------------------
 * Scroll and zoom over the integrated centreline.  Bends are coloured by
 * direction so the shape of the course reads at a glance, and the view
 * follows the scrub position when zoomed in.
 */
static void course_draw_map(Rectangle r)
{
    DrawRectangleRec(r, PANEL_BG);
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);

    float spanx = course_maxx - course_minx, spany = course_maxy - course_miny;
    if (spanx < 1) spanx = 1;
    if (spany < 1) spany = 1;
    float fit = (r.width - 60) / spanx;
    float fity = (r.height - 60) / spany;
    if (fity < fit) fit = fity;
    float s = fit * course_zoom;

    int at = (int)course_scrub;
    if (at > COURSE_SEGMENTS) at = COURSE_SEGMENTS;
    Vector2 focus = course_zoom > 1.01f
        ? course_path[at].p
        : (Vector2){(course_minx + course_maxx) * 0.5f,
                    (course_miny + course_maxy) * 0.5f};
    float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f;

    for (int i = 0; i < COURSE_SEGMENTS; i++) {
        Vector2 a = {cx + (course_path[i].p.x - focus.x) * s,
                     cy - (course_path[i].p.y - focus.y) * s};
        Vector2 b = {cx + (course_path[i + 1].p.x - focus.x) * s,
                     cy - (course_path[i + 1].p.y - focus.y) * s};
        int c = course_curve(i);
        Color col = c > 0 ? (Color){250, 160, 90, 255}
                  : c < 0 ? SKYBLUE : (Color){120, 130, 150, 255};
        DrawLineEx(a, b, course_zoom > 2 ? 5.0f : 3.0f, col);
    }
    Vector2 st = {cx + (course_path[0].p.x - focus.x) * s,
                  cy - (course_path[0].p.y - focus.y) * s};
    DrawCircleV(st, 7, (Color){61, 220, 132, 255});
    Vector2 sc = {cx + (course_path[at].p.x - focus.x) * s,
                  cy - (course_path[at].p.y - focus.y) * s};
    DrawCircleV(sc, 8, LABEL);
    DrawCircleLinesV(sc, 13, LABEL);
    EndScissorMode();
    DrawRectangleLinesEx(r, 1, GRID);

    char z[64];
    snprintf(z, sizeof z, "TOP DOWN   %s    curves    zoom x%.0f",
             course_name() ? course_name() : COURSES[course_sel].name,
             course_zoom);
    ui_text(z, (int)r.x + 10, (int)r.y + 8, 22, LABEL);
    ui_text("orange right   blue left   green start   yellow here",
            (int)r.x + 10, (int)(r.y + r.height - 26), 18, LIGHTGRAY);
}

/* ---- side on: the gradients -----------------------------------------
 * Height comes from integrating the slope byte the same way the game's
 * generator accumulates it, so uphill and downhill sections read as a
 * terrain silhouette.  The upper strip is a window around the current
 * position; the lower strip is the whole course.
 */
static void course_draw_profile(Rectangle r)
{
    DrawRectangleRec(r, PANEL_BG);
    int at = (int)course_scrub;
    const int WIN = 140;                    /* segments either side */
    int from = at - WIN / 4, to = from + WIN;
    if (from < 0) { from = 0; to = WIN; }
    if (to > COURSE_SEGMENTS) { to = COURSE_SEGMENTS; from = to - WIN; }
    if (from < 0) from = 0;

    float lo = 1e9f, hi = -1e9f;
    for (int i = from; i <= to; i++) {
        if (course_path[i].h < lo) lo = course_path[i].h;
        if (course_path[i].h > hi) hi = course_path[i].h;
    }
    float span = (hi - lo) > 0.001f ? (hi - lo) : 1;
    float base = r.y + r.height - 22;
    float top = r.y + 34;

    /* filled terrain silhouette */
    for (int i = from; i < to; i++) {
        float x0 = r.x + r.width * (i - from) / (float)(to - from);
        float x1 = r.x + r.width * (i + 1 - from) / (float)(to - from);
        float y0 = base - (course_path[i].h - lo) / span * (base - top);
        float y1 = base - (course_path[i + 1].h - lo) / span * (base - top);
        int sl = course_slope(i);
        Color fill = sl > 0 ? (Color){64, 104, 60, 255}
                   : sl < 0 ? (Color){50, 74, 96, 255}
                            : (Color){52, 60, 70, 255};
        DrawTriangle((Vector2){x0, y0}, (Vector2){x0, base},
                     (Vector2){x1, base}, fill);
        DrawTriangle((Vector2){x0, y0}, (Vector2){x1, base},
                     (Vector2){x1, y1}, fill);
        DrawLineEx((Vector2){x0, y0}, (Vector2){x1, y1}, 2.5f,
                   (Color){150, 200, 120, 255});
    }
    /* here marker */
    float mx = r.x + r.width * (at - from) / (float)(to - from);
    DrawLineEx((Vector2){mx, top - 8}, (Vector2){mx, base}, 2.0f, LABEL);
    DrawCircleV((Vector2){mx,
                base - (course_path[at].h - lo) / span * (base - top)},
                6, LABEL);

    DrawRectangleLinesEx(r, 1, GRID);
    char t[96];
    snprintf(t, sizeof t, "SIDE ON   %s   gradient    segments %d - %d",
             course_name() ? course_name() : COURSES[course_sel].name,
             from, to);
    ui_text(t, (int)r.x + 10, (int)r.y + 8, 22, LABEL);
    ui_text("green up   blue down", (int)(r.x + r.width - 220),
            (int)r.y + 8, 18, LIGHTGRAY);
}

/* whole-course strip: position within the track at a glance */
static void course_draw_strip(Rectangle r)
{
    DrawRectangleRec(r, BTN_IDLE);
    float lo = 0, hi = 0;
    for (int i = 0; i <= COURSE_SEGMENTS; i++) {
        if (course_path[i].h < lo) lo = course_path[i].h;
        if (course_path[i].h > hi) hi = course_path[i].h;
    }
    float span = (hi - lo) > 0.001f ? (hi - lo) : 1;
    for (int i = 0; i < COURSE_SEGMENTS; i++) {
        float x0 = r.x + r.width * i / (float)COURSE_SEGMENTS;
        float y0 = r.y + r.height - 4
                 - (course_path[i].h - lo) / span * (r.height - 10);
        int c = course_curve(i);
        Color col = c > 0 ? (Color){250, 160, 90, 255}
                  : c < 0 ? SKYBLUE : (Color){150, 200, 120, 255};
        DrawRectangle((int)x0, (int)y0, 2, 3, col);
    }
    float px = r.x + r.width * course_scrub / (float)COURSE_SEGMENTS;
    DrawRectangle((int)px - 2, (int)r.y, 4, (int)r.height, LABEL);
    DrawRectangleLinesEx(r, 1, BTN_EDGE);
}

static void page_course(void)
{
    char buf[192];
    course_integrate();
    /* Render every frame: the course table is only populated once the
     * game reaches a race, so caching on the scrub position alone left a
     * blank straight road on screen forever if the first render happened
     * before the data arrived. */
    /* The game's own road chain when a snapshot for this course is
     * available; the decoded-table preview only when it is not. */
    if (!rp_ready || rp_course != course_sel) roadplay_load(course_sel);
    if (rp_ready) roadplay_draw(course_scrub);
    else render_road_frame((int)course_scrub);
    course_rendered_at = course_scrub;

    const char *nm = course_name();
    char head[96];
    snprintf(head, sizeof head, "COURSE   %s", nm ? nm : "UNIDENTIFIED");
    page_head(head);
    int x0 = 16, y0 = 58;
    snprintf(buf, sizeof buf, "%.0f / %d", course_scrub, COURSE_SEGMENTS);
    kv(x0, y0, "segment", buf, 190);
    snprintf(buf, sizeof buf, "%dR  %dL  %d hills",
             course_turns_r, course_turns_l, course_hills);
    kv(x0, y0 + 26, "layout", buf, 190);
    int cc = course_curve((int)course_scrub);
    snprintf(buf, sizeof buf, "%s %d      slope %+d",
             cc > 0 ? "right" : cc < 0 ? "left " : "straight", cc,
             course_slope((int)course_scrub));
    kv(x0 + 560, y0, "here", buf, 96);
    snprintf(buf, sizeof buf, "$%08x   code %s", course_hash(),
             COURSES[course_sel].code);
    kv(x0 + 560, y0 + 26, "course id", buf, 96);

    /* track select: one button per level, greyed when not captured yet */
    for (int i = 0; i < COURSE_COUNT; i++) {
        Rectangle b = {16 + i * 152, 112, 146, 30};
        int have = 0;
        FILE *probe = fopen(COURSES[i].file, "rb");
        if (probe) { have = 1; fclose(probe); }
        if (!have) {
            /* a race snapshot is data enough: the road view and the 2D
             * views both read the course table out of it */
            char sp[256];
            snprintf(sp, sizeof sp,
                     "re/pipeline/courses/%s_0_211e78_fast.bin",
                     ROADPLAY_NAMES[i]);
            probe = fopen(sp, "rb");
            if (probe) { have = 1; fclose(probe); }
        }
        if (!have) {
            DrawRectangleRec(b, (Color){34, 34, 40, 255});
            DrawRectangleLinesEx(b, 1, (Color){60, 60, 68, 255});
            int fs = 20, tw = ui_measure(COURSES[i].name, fs);
            ui_text(COURSES[i].name, b.x + (b.width - tw) / 2,
                    b.y + (b.height - fs) / 2, fs, (Color){90, 90, 100, 255});
            continue;
        }
        if (button(b, COURSES[i].name, i == course_sel) && i != course_sel) {
            /* the .l2c is optional now; the snapshot carries the table */
            course_load(COURSES[i].file);
            course_sel = i;
            course_scrub = 0;
            /* keeps driving */;
            roadplay_load(i);
        }
    }

    /* three synchronised views */
    float paneW = (WIN_W - 48) * 0.5f;
    Rectangle v3d = {16, 152, paneW, 438};
    Rectangle map = {16 + paneW + 16, 152, paneW, 438};
    course_draw_3d(v3d);
    course_draw_map(map);
    course_draw_profile((Rectangle){16, 606, WIN_W - 32, 200});
    course_draw_strip((Rectangle){16, 818, WIN_W - 32, 34});

    /* --- navigation --- */
    Rectangle sb = {16, 818, WIN_W - 32, 34};
    if (ui_hit(sb) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        course_scrub = (mpos().x - sb.x) / sb.width * COURSE_SEGMENTS;
        /* keeps driving */;
    }

    /* Always driving.  Scrubbing is how you choose WHERE on the course
     * to be; it should not also have to be how you make the road move,
     * so there is no separate DRIVE toggle any more. */
    /* Race it, without going near the password screen. */
    {
        Rectangle rb = {WIN_W - 520, 112, 240, 30};
        Rectangle sb = {WIN_W - 270, 112, 250, 30};
        char lbl[64];
        snprintf(lbl, sizeof lbl, "RACE %s", COURSES[course_sel].name);
        if (button(rb, lbl, 0)) sel_request = course_sel + 1;
        /* The series is what the game does on its own: finish a stage
         * and it moves to the next.  So this is "start at the
         * beginning", and the game handles the rest -- there is no
         * sequencing here pretending to be a feature. */
        if (button(sb, "RACE FROM THE START", sel_series))
            sel_request = -1;
    }

    float cy = 866, ch = 44;
    ui_text("SPEED", 20, (int)cy + 12, 20, LABEL);
    static const float SPEEDS[] = {SEG_PER_FRAME * 0.25f,
                                   SEG_PER_FRAME * 0.5f,
                                   SEG_PER_FRAME,
                                   SEG_PER_FRAME * 2.0f,
                                   SEG_PER_FRAME * 4.0f};
    static const char *SPEED_NAMES[] = {"x.25", "x.5", "x1", "x2", "x4"};
    for (int k = 0; k < 5; k++) {
        int on = course_speed > SPEEDS[k] * 0.99f &&
                 course_speed < SPEEDS[k] * 1.01f;
        if (button((Rectangle){210 + k * 70, cy, 64, ch}, SPEED_NAMES[k], on))
            course_speed = SPEEDS[k];
    }
    if (button((Rectangle){590, cy, 60, ch}, "|<", 0))
        { course_scrub = 0; /* keeps driving */; }
    if (button((Rectangle){654, cy, 60, ch}, "<<", 0))
        { course_scrub -= 25; /* keeps driving */; }
    if (button((Rectangle){718, cy, 60, ch}, "<", 0))
        { course_scrub -= 5; /* keeps driving */; }
    if (button((Rectangle){782, cy, 60, ch}, ">", 0))
        { course_scrub += 5; /* keeps driving */; }
    if (button((Rectangle){846, cy, 60, ch}, ">>", 0))
        { course_scrub += 25; /* keeps driving */; }
    if (button((Rectangle){910, cy, 60, ch}, ">|", 0))
        { course_scrub = COURSE_SEGMENTS - 1; /* keeps driving */; }
    if (button((Rectangle){WIN_W - 130, cy, 114, ch}, "ZOOM", 0)) {
        course_zoom *= 2.0f;
        if (course_zoom > 8.5f) course_zoom = 1.0f;
    }
    ui_text("it drives by itself   -   pad: stick scrubs, shoulders jump, "
            "B zooms the map     keys: arrows, shift+arrows, HOME/END",
            16, (int)cy + ch + 12, 18, LIGHTGRAY);

    /* --- joystick navigation --------------------------------------
     * Through pad.c's own /dev/input/js reader, not raylib's gamepad
     * API: pad.c opens the device directly and raylib may never see it,
     * which is why the pad did nothing here before.
     */
    if (js_present(0)) {
        uint8_t st = gamepad_stick(0);
        if (st & 0x04) course_scrub -= SEG_PER_FRAME * 8.0f;
        if (st & 0x08) course_scrub += SEG_PER_FRAME * 8.0f;
        if (js_edge(0, 1)) {                   /* B cycles the map zoom */
            course_zoom *= 2.0f;
            if (course_zoom > 8.5f) course_zoom = 1.0f;
        }
    }

    /* keyboard navigation */
    /* held arrows scrub; a plain press moves at about forty times race
     * speed, shift covers ground */
    float step = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
               ? 10.0f : 0.5f;
    if (IsKeyDown(KEY_LEFT))  { course_scrub -= step; /* keeps driving */; }
    if (IsKeyDown(KEY_RIGHT)) { course_scrub += step; /* keeps driving */; }
    if (IsKeyPressed(KEY_HOME)) { course_scrub = 0; /* keeps driving */; }
    if (IsKeyPressed(KEY_END))
        { course_scrub = COURSE_SEGMENTS - 1; /* keeps driving */; }
    float wheel = GetMouseWheelMove();
    if (wheel && ui_hit(map)) {
        course_zoom *= wheel > 0 ? 1.25f : 0.8f;
        if (course_zoom < 1.0f) course_zoom = 1.0f;
        if (course_zoom > 16.0f) course_zoom = 16.0f;
    }
    course_scrub += course_speed;
    if (course_scrub < 0) course_scrub = 0;
    if (course_scrub > COURSE_SEGMENTS - 1) course_scrub = 0;
}

/* ---- GRAPHICS: what is actually in chip RAM ---------------------------
 * A planar bitmap viewer over the guest's chip RAM, decoded with the
 * live copper palette.  The loading and course-intro pictures are
 * ordinary bitplanes in chip RAM like everything else, so pointing this
 * at the screen buffers shows them; the address, plane count, width and
 * row stride are all adjustable because a picture's geometry is not
 * something to guess at.
 */
#define GFX_MAX_W 512
#define GFX_MAX_H 320
static uint32_t gfx_img[GFX_MAX_W * GFX_MAX_H];
static Texture2D gfx_tex;
static uint32_t gfx_addr = 0x10186;
static int gfx_planes = 4, gfx_words = 20, gfx_rows = 200;
static int gfx_stride = 42, gfx_plane_gap = 0x20d0;
static int gfx_from_copper = 1;

/* Known places to look, read from the base page rather than hardcoded. */
static void gfx_take_buffer(uint32_t at)
{
    uint32_t v = r32(at);
    if (v >= 0x400 && v < CHIP_SIZE) gfx_addr = v & ~1u;
}

static void gfx_decode(void)
{
    uint16_t pal[32];
    amiga_get_palette(pal);
    int w = gfx_words * 16;
    if (w > GFX_MAX_W) w = GFX_MAX_W;
    int h = gfx_rows > GFX_MAX_H ? GFX_MAX_H : gfx_rows;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned idx = 0;
            for (int p = 0; p < gfx_planes; p++) {
                uint32_t at = gfx_addr + (uint32_t)p * gfx_plane_gap
                            + (uint32_t)y * gfx_stride + (uint32_t)(x >> 3);
                if (at >= CHIP_SIZE) continue;
                if ((chip[at] >> (7 - (x & 7))) & 1) idx |= 1u << p;
            }
            uint16_t c = pal[idx & 31];
            gfx_img[y * GFX_MAX_W + x] =
                0xff000000u | (uint32_t)(((c >> 8) & 15) * 17)
                | ((uint32_t)(((c >> 4) & 15) * 17) << 8)
                | ((uint32_t)((c & 15) * 17) << 16);
        }
    for (int y = h; y < GFX_MAX_H; y++)
        for (int x = 0; x < GFX_MAX_W; x++) gfx_img[y * GFX_MAX_W + x] = 0xff000000u;
    for (int y = 0; y < h; y++)
        for (int x = w; x < GFX_MAX_W; x++) gfx_img[y * GFX_MAX_W + x] = 0xff000000u;
}

/* Read the geometry out of the copper list that is actually on screen.
 *
 * The page used to open with the RACING geometry -- four planes, a row
 * stride of 42, planes $20d0 apart -- and a loading screen is not that
 * shape, so it came out as hash.  A picture's geometry is not something
 * to guess at when the machine is holding the answer: the copper list
 * the chipset is running carries the bitplane pointers, the modulo and
 * the plane count.  COP1LC says which list that is, so there is no
 * matching or guessing involved.
 *
 * Returns 1 when it found a usable list.
 */
static uint32_t gfx_coplc;
static int gfx_repointed;   /* the copper moves the planes mid-frame */
static int gfx_export;      /* EXPORT was clicked this frame */
static int gfx_read_display(void)
{
    uint32_t at = amiga_get_coplc() & (CHIP_SIZE - 1);
    if (at < 0x400) return 0;
    gfx_coplc = at;
    uint32_t ptr[6] = {0};
    int planes = 0, mod1 = 0, have = 0, repoint = 0;
    unsigned seen_ptr = 0;
    for (int i = 0; i < 4096; i++, at += 4) {
        if (at + 3 >= CHIP_SIZE) break;
        uint16_t reg = (uint16_t)((chip[at] << 8) | chip[at + 1]);
        uint16_t dat = (uint16_t)((chip[at + 2] << 8) | chip[at + 3]);
        if (reg >= 0x00e0 && reg <= 0x00f3) {
            /* The FIRST write wins.  Lotus re-points the bitplanes from
             * the copper part-way down the screen -- that is how the
             * road is drawn -- so taking the last write starts the raw
             * view somewhere in the middle of the picture, which is what
             * made the planes look like garbage while the composited
             * view beside them was fine. */
            int p = (reg - 0x00e0) / 4;
            if (!(seen_ptr & (1u << (p * 2 + ((reg & 2) ? 1 : 0))))) {
                if (reg & 2) ptr[p] = (ptr[p] & 0xffff0000u) | dat;
                else ptr[p] = (ptr[p] & 0xffffu) | ((uint32_t)dat << 16);
                seen_ptr |= 1u << (p * 2 + ((reg & 2) ? 1 : 0));
            }
            if (p + 1 > have) have = p + 1;
            repoint++;
        } else if (reg == 0x0100) {
            planes = (dat >> 12) & 7;
        } else if (reg == 0x0108) {
            mod1 = (int16_t)dat;
        } else if (reg == 0xffff && dat == 0xfffe) {
            break;
        }
    }
    if (!have || (ptr[0] & ~1u) < 0x400) return 0;
    /* More pointer writes than planes means the copper moves them
     * during the frame, and no single stride describes the result. */
    gfx_repointed = repoint > have * 2;
    gfx_addr = ptr[0] & ~1u;
    gfx_planes = planes >= 1 && planes <= 6 ? planes
               : (have >= 1 && have <= 6 ? have : 4);
    gfx_stride = 40 + (mod1 > 0 ? mod1 : 0);
    gfx_words = 20;
    if (have > 1 && ptr[1] > ptr[0] && ptr[1] - ptr[0] < CHIP_SIZE)
        gfx_plane_gap = (int)(ptr[1] - ptr[0]);
    else
        gfx_plane_gap = gfx_stride * gfx_rows;
    return 1;
}

static void page_gfx(void)
{
    page_head("GRAPHICS");
    char buf[128];

    /* Follow the display until someone touches a control, so the page
     * opens showing the screen that is up rather than a guess. */
    if (gfx_from_copper) gfx_read_display();

    /* the game's own idea of where its screens are */
    static const struct { const char *name; uint32_t at; } SLOTS[] = {
        {"SHOWN",  A3 + 0x2f8a}, {"DRAWN", A3 + 0x2f8e},
        {"THIRD",  A3 + 0x2f92}, {"SCR A", A3 + 0x2fd6},
        {"SCR B",  A3 + 0x2fda},
    };
    for (int i = 0; i < 5; i++) {
        Rectangle b = {16 + i * 132, 64, 126, 34};
        uint32_t v = r32(SLOTS[i].at) & ~1u;
        int on = (v == gfx_addr) || ((v + 2) == gfx_addr);
        snprintf(buf, sizeof buf, "%s", SLOTS[i].name);
        if (button(b, buf, on)) { gfx_take_buffer(SLOTS[i].at); gfx_from_copper = 0; }
    }

    /* geometry, because a picture's shape is not a thing to guess */
    int bx = 16, by = 110, bh = 32;
    snprintf(buf, sizeof buf, "$%06x", gfx_addr);
    ui_text("ADDR", bx, by + 6, 20, LABEL);
    ui_text(buf, bx + 60, by + 6, 20, RAYWHITE);
    if (button((Rectangle){bx + 150, by, 44, bh}, "-1k", 0))
        { gfx_addr = gfx_addr > 0x400 ? gfx_addr - 0x400 : 0; gfx_from_copper = 0; }
    if (button((Rectangle){bx + 198, by, 44, bh}, "+1k", 0))
        { gfx_addr += 0x400; gfx_from_copper = 0; }
    if (button((Rectangle){bx + 246, by, 50, bh}, "-row", 0))
        { gfx_addr = gfx_addr > (uint32_t)gfx_stride ? gfx_addr - gfx_stride : 0;
          gfx_from_copper = 0; }
    if (button((Rectangle){bx + 300, by, 50, bh}, "+row", 0))
        { gfx_addr += gfx_stride; gfx_from_copper = 0; }

    ui_text("PLANES", bx + 380, by + 6, 20, LABEL);
    for (int p = 1; p <= 6; p++) {
        snprintf(buf, sizeof buf, "%d", p);
        if (button((Rectangle){bx + 460 + (p - 1) * 40, by, 36, bh}, buf,
                   p == gfx_planes)) { gfx_planes = p; gfx_from_copper = 0; }
    }
    ui_text("STRIDE", bx + 720, by + 6, 20, LABEL);
    static const int STRIDES[] = {40, 42, 44, 48, 80};
    for (int i = 0; i < 5; i++) {
        snprintf(buf, sizeof buf, "%d", STRIDES[i]);
        if (button((Rectangle){bx + 800 + i * 52, by, 48, bh}, buf,
                   gfx_stride == STRIDES[i]))
            { gfx_stride = STRIDES[i]; gfx_from_copper = 0; }
    }
    ui_text("WIDTH", bx + 1080, by + 6, 20, LABEL);
    static const int WORDS[] = {16, 20, 22, 24, 32};
    for (int i = 0; i < 5; i++) {
        snprintf(buf, sizeof buf, "%d", WORDS[i] * 16);
        if (button((Rectangle){bx + 1160 + i * 62, by, 58, bh}, buf,
                   gfx_words == WORDS[i]))
            { gfx_words = WORDS[i]; gfx_from_copper = 0; }
    }
    snprintf(buf, sizeof buf, "plane gap $%04x", gfx_plane_gap);
    ui_text(buf, bx + 1490, by + 6, 20, LABEL);
    if (button((Rectangle){bx + 1660, by, 60, bh}, "$20d0", gfx_plane_gap == 0x20d0))
        { gfx_plane_gap = 0x20d0; gfx_from_copper = 0; }
    if (button((Rectangle){bx + 1726, by, 60, bh}, "1 pic",
               gfx_plane_gap == gfx_stride * gfx_rows))
        { gfx_plane_gap = gfx_stride * gfx_rows; gfx_from_copper = 0; }
    if (button((Rectangle){bx + 1792, by, 110, bh}, "FOLLOW SCREEN",
               gfx_from_copper)) gfx_from_copper = 1;
    /* Save what is on this page.  There is no folder of game art in the
     * repository and there is not going to be one -- it is the retail
     * game's -- so ripping is something you do, into a directory that is
     * gitignored, rather than something shipped. */
    if (button((Rectangle){bx + 1908, by, 100, bh}, "EXPORT", 0))
        gfx_export = 1;

    /* The screen as the machine shows it: the compositor is a per-line
     * copper interpreter that make render-gate proves pixel-identical to
     * the oracle, so this is the picture, not an interpretation of it. */
    static uint32_t shot[VIEW_W * VIEW_H];
    static Texture2D shot_tex;
    static int shot_ready;
    if (!shot_ready) {
        Image si = { .data = shot, .width = VIEW_W, .height = VIEW_H,
                     .mipmaps = 1,
                     .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
        shot_tex = LoadTextureFromImage(si);
        shot_ready = 1;
    }
    int have_shot = gfx_coplc && composite(chip, gfx_coplc, shot) == 0;
    if (have_shot) UpdateTexture(shot_tex, shot);

    Rectangle live = {WIN_W - 16 - 640, 156, 640, 400};
    if (have_shot) {
        DrawRectangleRec(live, PANEL_BG);
        DrawTexturePro(shot_tex, (Rectangle){0, 0, VIEW_W, VIEW_H},
                       (Rectangle){live.x + 8, live.y + 32, live.width - 16,
                                   (live.width - 16) * 200.0f / 320.0f},
                       (Vector2){0, 0}, 0.0f, WHITE);
        DrawRectangleLinesEx(live, 1, GRID);
        snprintf(buf, sizeof buf, "ON SCREEN   copper list $%06x", gfx_coplc);
        ui_text(buf, (int)live.x + 8, (int)live.y + 6, 20, LABEL);
    }

    gfx_decode();
    UpdateTexture(gfx_tex, gfx_img);

    if (gfx_export) {
        gfx_export = 0;
        char path[256];
        int w = gfx_words * 16 > GFX_MAX_W ? GFX_MAX_W : gfx_words * 16;
        int h = gfx_rows > GFX_MAX_H ? GFX_MAX_H : gfx_rows;
        /* crop out of the padded working buffer, so the file is the
         * picture and not the buffer it was decoded into */
        static uint32_t crop[GFX_MAX_W * GFX_MAX_H];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                crop[y * w + x] = gfx_img[y * GFX_MAX_W + x];
        Image im = { .data = crop, .width = w, .height = h, .mipmaps = 1,
                     .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
        snprintf(path, sizeof path, "graphics/planes_%06x_%dp_%dx%d.png",
                 gfx_addr, gfx_planes, w, h);
        ExportImage(im, path);
        if (have_shot) {
            Image si = { .data = shot, .width = VIEW_W, .height = VIEW_H,
                         .mipmaps = 1,
                         .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
            snprintf(path, sizeof path, "graphics/screen_%06x.png",
                     gfx_coplc);
            ExportImage(si, path);
        }
    }
    Rectangle area = {16, 156, WIN_W - 32 - (have_shot ? 656 : 0),
                      WIN_H - BAR_H - 176};
    /* the caption gets its own strip: over the picture it sat on the
     * game's own HUD and neither could be read */
    const int GCAP = 28;
    DrawRectangleRec(area, PANEL_BG);
    Rectangle pic = {area.x, area.y + GCAP, area.width, area.height - GCAP};
    float sc = pic.width / (float)(gfx_words * 16);
    float sy = pic.height / (float)gfx_rows;
    if (sy < sc) sc = sy;
    Rectangle dst = {pic.x + (pic.width - gfx_words * 16 * sc) / 2,
                     pic.y + (pic.height - gfx_rows * sc) / 2,
                     gfx_words * 16 * sc, gfx_rows * sc};
    DrawTexturePro(gfx_tex,
                   (Rectangle){0, 0, (float)(gfx_words * 16), (float)gfx_rows},
                   dst, (Vector2){0, 0}, 0.0f, WHITE);
    DrawRectangleLinesEx(area, 1, GRID);
    /* One palette for the whole picture, because that is what a bitmap
     * has.  The screen beside it can differ in colour and still be the
     * same data: the copper rewrites the palette per line, which is how
     * the sky gets its gradient. */
    snprintf(buf, sizeof buf, "BITPLANES   one palette%s",
             gfx_repointed ? "; the copper also moves these planes "
                             "mid-frame, so one stride shows part of "
                             "the screen" : "");
    ui_text(buf, (int)area.x + 8, (int)area.y + 5, 20, LABEL);
}

/* ---- SOUND: the four Paula voices ------------------------------------
 * Everything here is read straight out of the chipset state: location,
 * length, period and volume as the game programmed them, and the sample
 * each voice is playing, drawn from chip RAM as signed bytes.  Period
 * converts to pitch with the PAL colour clock, 3546895 / period.
 */
static void page_sound(void)
{
    page_head("SOUND");
    char buf[192];
    float top = 74, rowh = (WIN_H - BAR_H - 104) / 4.0f;

    for (int c = 0; c < 4; c++) {
        AmigaVoice v;
        amiga_get_voice(c, &v);
        Rectangle r = {16, top + c * rowh, WIN_W - 32, rowh - 12};
        DrawRectangleRec(r, PANEL_BG);
        DrawRectangleLinesEx(r, 1, v.on ? (Color){90, 200, 120, 255} : GRID);

        float hz = v.period ? 3546895.0f / v.period : 0.0f;
        snprintf(buf, sizeof buf,
                 "VOICE %d   %s   lc $%06x   playing $%06x   %u words   "
                 "period %u  (%.0f Hz)   volume %u/64",
                 c, v.on ? "ON " : "off", v.lc, v.lc_play, v.lenlatch,
                 v.period, hz, v.volume);
        ui_text(buf, (int)r.x + 10, (int)r.y + 8, 20,
                v.on ? RAYWHITE : (Color){130, 130, 140, 255});

        /* Listening controls.  MUTE silences this voice in the mixer;
         * SOLO silences the other three.  Neither touches the guest --
         * the game plays on and the chipset state is untouched, so what
         * you hear changes and nothing else does. */
        int muted = amiga_voice_muted(c);
        Rectangle mb = {r.x + r.width - 230, r.y + 6, 100, 30};
        Rectangle sb = {r.x + r.width - 120, r.y + 6, 100, 30};
        if (button(mb, muted ? "UNMUTE" : "MUTE", muted))
            amiga_voice_mute(c, !muted);
        int solo = !muted;
        for (int k = 0; k < 4 && solo; k++)
            if (k != c && !amiga_voice_muted(k)) solo = 0;
        if (button(sb, "SOLO", solo)) {
            if (solo) for (int k = 0; k < 4; k++) amiga_voice_mute(k, 0);
            else      for (int k = 0; k < 4; k++) amiga_voice_mute(k, k != c);
        }

        /* What the voice actually put into the mix, newest at the right.
         * Drawing the sample sitting in chip RAM instead was useless:
         * once a one-shot has played, the game points the channel at a
         * two-byte silent loop, and a two-byte waveform is a flat line. */
        Rectangle w = {r.x + 10, r.y + 44, r.width - 20, r.height - 54};
        DrawRectangleRec(w, (Color){14, 14, 20, 255});
        static int16_t scope[2048];
        int n = amiga_voice_scope(c, scope, 2048);
        float mid = w.y + w.height * 0.5f;
        DrawLineEx((Vector2){w.x, mid}, (Vector2){w.x + w.width, mid}, 1.0f,
                   (Color){40, 40, 50, 255});
        if (n > 1) {
            int cols = (int)w.width;
            Color line = muted ? (Color){90, 90, 100, 255}
                       : v.on  ? (Color){120, 220, 160, 255}
                               : (Color){80, 80, 90, 255};
            for (int x = 0; x < cols; x++) {
                int i0 = (int)((float)x / cols * n);
                int i1 = (int)((float)(x + 1) / cols * n);
                if (i1 <= i0) i1 = i0 + 1;
                int lo = 32767, hi = -32768;
                for (int i = i0; i < i1 && i < n; i++) {
                    if (scope[i] < lo) lo = scope[i];
                    if (scope[i] > hi) hi = scope[i];
                }
                if (hi < lo) continue;
                float y0 = mid - hi / 8192.0f * (w.height * 0.5f - 2);
                float y1 = mid - lo / 8192.0f * (w.height * 0.5f - 2);
                if (y0 < w.y) y0 = w.y;
                if (y1 > w.y + w.height) y1 = w.y + w.height;
                DrawLineEx((Vector2){w.x + x, y0}, (Vector2){w.x + x, y1},
                           1.0f, line);
            }
        }
    }
    ui_text("MUTE silences one voice, SOLO silences the other three -- "
            "in the mix only; the game is untouched",
            16, WIN_H - BAR_H - 26, 18, LIGHTGRAY);
}

/* ---- start any course, without typing a password ---------------------
 * Every course past the first is behind a password, and the front end
 * knows all eight.  Rather than poke the game's state -- which skips
 * whatever else its own setup does -- this REPLAYS the password: reboot,
 * feed the recorded stick and the recorded keystrokes, and let go once
 * the recording ends.  The game does its own work; it just does not need
 * anyone at the keyboard.
 *
 * The wait is real but short.  Boot and the menus run uncapped, which is
 * about ten seconds for the six thousand frames it takes to reach the
 * start line, and the frame rate drops back to 50 the moment the
 * recording hands over.
 */
typedef struct { long frame; unsigned code; } KeyEvent;
static uint8_t *sel_rec;            /* one stick byte per frame */
static long sel_rec_len, sel_rec_at;
static KeyEvent sel_keys[256];
static int sel_key_count, sel_key_at;
static int sel_running;             /* replaying: the guest drives itself */
static int sel_course = -1;

static void sel_stop(void)
{
    free(sel_rec);
    sel_rec = NULL;
    sel_rec_len = sel_rec_at = 0;
    sel_key_count = sel_key_at = 0;
    sel_running = 0;
}

/* Load a course's recorded password session.  FOREST has no password and
 * no recording: it is what the game starts on, so it only needs the
 * menus driven, which the recording for any course also does. */
static int sel_load(int course)
{
    static const char *const NAMES[COURSE_COUNT] = {
        "forest", "night", "fog", "snow", "desert", "motorway", "marsh",
        "storm"
    };
    if (course < 0 || course >= COURSE_COUNT) return 0;
    sel_stop();
    char path[256];
    snprintf(path, sizeof path, "re/pipeline/courses/%s.rec", NAMES[course]);
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* FOREST: no password to type, so drive the menus with fire */
        if (course != 0) return 0;
        sel_rec_len = 3200;
        sel_rec = calloc(1, sel_rec_len);
        if (!sel_rec) return 0;
        for (long i = 2100; i < sel_rec_len; i += 100)
            for (long k = 0; k < 8 && i + k < sel_rec_len; k++)
                sel_rec[i + k] = 0x10;
        sel_course = course;
        sel_running = 1;
        return 1;
    }
    fseek(f, 0, SEEK_END);
    sel_rec_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    sel_rec = malloc(sel_rec_len ? sel_rec_len : 1);
    if (sel_rec && sel_rec_len)
        sel_rec_len = (long)fread(sel_rec, 1, sel_rec_len, f);
    fclose(f);

    snprintf(path, sizeof path, "re/pipeline/courses/%s.keys", NAMES[course]);
    f = fopen(path, "r");
    if (f) {
        long fr; unsigned code;
        while (sel_key_count < 256 && fscanf(f, "%ld %x", &fr, &code) == 2) {
            sel_keys[sel_key_count].frame = fr;
            sel_keys[sel_key_count].code = code;
            sel_key_count++;
        }
        fclose(f);
    }
    sel_course = course;
    sel_running = 1;
    return sel_rec != NULL;
}

/* Reboot and start replaying.  Returns 0 if the recording is missing. */
static int sel_start(int course, WhdConfig *whd)
{
    if (!sel_load(course)) return 0;
    amiga_init();
    if (!whdload_boot(whd)) { sel_stop(); return 0; }
    amiga_enable_video(true);
    sel_rec_at = 0;
    sel_key_at = 0;
    SetTargetFPS(0);                /* the menus do not need to be watched */
    return 1;
}

/* One frame of the replay.  Returns the stick to feed the guest, and
 * clears `sel_running` when the recording runs out. */
static uint8_t sel_frame(void)
{
    if (sel_key_at < sel_key_count &&
        sel_keys[sel_key_at].frame <= sel_rec_at && amiga_kbd_idle()) {
        amiga_key_event((uint8_t)sel_keys[sel_key_at].code, false);
        sel_key_at++;
    }
    uint8_t b = (sel_rec_at < sel_rec_len) ? sel_rec[sel_rec_at] : 0;
    sel_rec_at++;
    if (sel_rec_at >= sel_rec_len) {
        sel_running = 0;
        SetTargetFPS(50);           /* over to the player */
    }
    return b;
}

/* ---- in-game course map ---------------------------------------------
 * The same integrated centreline the preview page draws, in the corner
 * of the player 2 panel while a race is on, with the car where the
 * game's own course position says it is.
 */
static void draw_race_map(Rectangle r)
{
    DrawRectangleRec(r, (Color){12, 12, 18, 255});
    DrawRectangleLinesEx(r, 1, (Color){70, 70, 82, 255});

    float spanx = course_maxx - course_minx, spany = course_maxy - course_miny;
    if (spanx < 1) spanx = 1;
    if (spany < 1) spany = 1;
    float s = (r.width - 16) / spanx;
    if ((r.height - 16) / spany < s) s = (r.height - 16) / spany;
    float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f;
    float fx = (course_minx + course_maxx) * 0.5f;
    float fy = (course_miny + course_maxy) * 0.5f;

    for (int i = 0; i < COURSE_SEGMENTS; i++) {
        Vector2 a = {cx + (course_path[i].p.x - fx) * s,
                     cy - (course_path[i].p.y - fy) * s};
        Vector2 b = {cx + (course_path[i + 1].p.x - fx) * s,
                     cy - (course_path[i + 1].p.y - fy) * s};
        int c = course_curve(i);
        Color col = c > 0 ? (Color){250, 160, 90, 255}
                  : c < 0 ? (Color){90, 170, 250, 255}
                          : (Color){150, 200, 120, 255};
        DrawLineEx(a, b, 2.0f, col);
    }
    /* where the car is, from the game's own 16.16 course position */
    float here = r32(A3 + 0x30d8) / 65536.0f;
    int i = (int)here;
    if (i < 0) i = 0;
    if (i > COURSE_SEGMENTS - 1) i = COURSE_SEGMENTS - 1;
    float t = here - i;
    Vector2 p = {cx + (course_path[i].p.x
                       + (course_path[i + 1].p.x - course_path[i].p.x) * t
                       - fx) * s,
                 cy - (course_path[i].p.y
                       + (course_path[i + 1].p.y - course_path[i].p.y) * t
                       - fy) * s};
    DrawCircleV(p, 5, (Color){255, 238, 136, 255});
    DrawCircleLinesV(p, 7, RAYWHITE);
}

/* ---- upscaling the picture ------------------------------------------
 * Scale3x (AdvMAME3x): for each pixel, look at its four orthogonal
 * neighbours and fill a 3x3 block, rounding a corner only where two
 * adjacent neighbours agree and the diagonal does not.  It invents no
 * colours -- every output pixel is one of the input pixels -- so the
 * game's palette survives, which bilinear smoothing does not manage.
 *
 * This cannot add detail the game never drew.  It cleans up the
 * diagonals in the art; the road is still rasterised at 320 wide, and
 * making THAT sharper is a different job (a renderer that draws the
 * road's own edge stream at a higher resolution, rather than a filter
 * over the result).
 */
enum { UP_SHARP, UP_SCALE3X, UP_SMOOTH, UP_COUNT };
static const char *const UP_NAME[UP_COUNT] = { "SHARP", "SCALE3X", "SMOOTH" };
static int up_mode = UP_SCALE3X;
static uint32_t up_buf[GAME_W * 3 * GAME_H * 3];
static Texture2D up_tex;

static void scale3x(const uint32_t *in, int inw, int x0, int y0,
                    int w, int h, uint32_t *out)
{
    const int ow = w * 3;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            #define PX(dx, dy) in[(y0 + (y) + (dy)) * inw + (x0 + (x) + (dx))]
            uint32_t e = PX(0, 0);
            uint32_t b = y > 0 ? PX(0, -1) : e;
            uint32_t h_ = y < h - 1 ? PX(0, 1) : e;
            uint32_t d = x > 0 ? PX(-1, 0) : e;
            uint32_t f = x < w - 1 ? PX(1, 0) : e;
            uint32_t a = (x > 0 && y > 0) ? PX(-1, -1) : e;
            uint32_t c = (x < w - 1 && y > 0) ? PX(1, -1) : e;
            uint32_t g = (x > 0 && y < h - 1) ? PX(-1, 1) : e;
            uint32_t i = (x < w - 1 && y < h - 1) ? PX(1, 1) : e;
            #undef PX
            uint32_t o[9];
            for (int k = 0; k < 9; k++) o[k] = e;
            if (d == b && d != h_ && b != f) o[0] = d;
            if ((d == b && d != h_ && b != f && e != c) ||
                (b == f && b != d && f != h_ && e != a)) o[1] = b;
            if (b == f && b != d && f != h_) o[2] = f;
            if ((h_ == d && h_ != f && d != b && e != a) ||
                (d == b && d != h_ && b != f && e != g)) o[3] = d;
            if ((b == f && b != d && f != h_ && e != i) ||
                (f == h_ && f != b && h_ != d && e != c)) o[5] = f;
            if (h_ == d && h_ != f && d != b) o[6] = d;
            if ((f == h_ && f != b && h_ != d && e != g) ||
                (h_ == d && h_ != f && d != b && e != i)) o[7] = h_;
            if (f == h_ && f != b && h_ != d) o[8] = f;
            for (int k = 0; k < 9; k++)
                out[(y * 3 + k / 3) * ow + x * 3 + k % 3] = o[k];
        }
    }
}

/* Draw the game window into `dst` at the chosen quality. */
static void draw_game_picture(Rectangle dst)
{
    /* below the HUD band, so the speed bar keeps its own red */
    car_recolour(framebuf, SCREEN_W, SCREEN_H, GAME_OY + 34);

    Rectangle photo;
    if (road_extras && intro_photo(&photo)) {
        SetTextureFilter(game_screen, TEXTURE_FILTER_BILINEAR);
        DrawTexturePro(game_screen, photo, dst, (Vector2){0, 0}, 0.0f, WHITE);
        return;
    }

    if (up_mode == UP_SCALE3X) {
        scale3x(framebuf, SCREEN_W, GAME_OX, GAME_OY, GAME_W, GAME_H, up_buf);
        if (!up_tex.id) {
            Image im = { .data = up_buf, .width = GAME_W * 3,
                         .height = GAME_H * 3, .mipmaps = 1,
                         .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
            up_tex = LoadTextureFromImage(im);
            SetTextureFilter(up_tex, TEXTURE_FILTER_BILINEAR);
        }
        UpdateTexture(up_tex, up_buf);
        DrawTexturePro(up_tex,
                       (Rectangle){0, 0, GAME_W * 3, GAME_H * 3}, dst,
                       (Vector2){0, 0}, 0.0f, WHITE);
        return;
    }
    SetTextureFilter(game_screen, up_mode == UP_SMOOTH
                     ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
    DrawTexturePro(game_screen,
                   (Rectangle){GAME_OX, GAME_OY, GAME_W, GAME_H}, dst,
                   (Vector2){0, 0}, 0.0f, WHITE);
}

/* ---- the start menu --------------------------------------------------
 * Press fire on the title and the front end asks which course, rather
 * than the game asking for a password.  It only intercepts fire while no
 * race is up -- during a race fire is the accelerator and taking it
 * would be worse than not having the menu at all.
 */
static int start_menu;
static int start_sel;          /* the highlighted entry */

/* Driven with the stick, because that is what is in your hand when the
 * game asks you to press fire.  The mouse still works; it is just not
 * required, which it was, which was the problem. */
static void start_menu_input(uint8_t stick)
{
    const int n = COURSE_COUNT + 2;
    static uint8_t was;
    uint8_t went = (uint8_t)(stick & ~was);
    was = stick;

    if ((went & 0x01) || IsKeyPressed(KEY_UP))    start_sel--;
    if ((went & 0x02) || IsKeyPressed(KEY_DOWN))  start_sel++;
    if (start_sel < 0) start_sel = n - 1;
    if (start_sel >= n) start_sel = 0;

    if ((went & 0x10) || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER)) {
        if (start_sel < COURSE_COUNT)      sel_request = start_sel + 1;
        else if (start_sel == COURSE_COUNT) sel_request = -1;
        else sel_request = 1 + (int)(wx_rand() * COURSE_COUNT) % COURSE_COUNT;
        start_menu = 0;
    }
}

/* Drawn to look like the game's own screens rather than like a tool:
 * the panel sits in the middle of the picture, black with a white rule
 * and a red heading, and the selected line is a solid red bar with the
 * text knocked out of it.  Those are the game's colours, taken from its
 * own palette ($a00 red, $ccc white, $000 black), so it sits inside the
 * picture instead of on top of it.
 */
static void start_menu_draw(Rectangle game)
{
    if (!start_menu) return;
    const int n = COURSE_COUNT + 2;
    /* raylib already owns the names RED and WHITE, as it owned PINK and
     * GOLD before them */
    const Color MRED   = { 170, 0, 0, 255 };
    const Color MWHITE = { 204, 204, 204, 255 };

    float scale = game.height / 200.0f;      /* the game's own pixel size */
    int lh = (int)(11 * scale);
    int w  = (int)(150 * scale);
    int h  = lh * (n + 2) + (int)(8 * scale);
    int x0 = (int)(game.x + (game.width - w) / 2);
    int y0 = (int)(game.y + (game.height - h) / 2);

    DrawRectangle(x0, y0, w, h, (Color){0, 0, 0, 235});
    DrawRectangleLinesEx((Rectangle){x0, y0, w, h}, (int)(2 * scale), MWHITE);

    int fs = (int)(9 * scale);
    if (fs < 10) fs = 10;
    int tw = ui_measure("SELECT COURSE", fs);
    ui_text("SELECT COURSE", x0 + (w - tw) / 2, y0 + (int)(6 * scale),
            fs, MRED);

    for (int i = 0; i < n; i++) {
        const char *label = i < COURSE_COUNT ? COURSES[i].name
                          : (i == COURSE_COUNT ? "ALL COURSES" : "SHUFFLE");
        int y = y0 + (int)(6 * scale) + lh * (i + 2);
        Rectangle line = {x0 + (int)(4 * scale), y - 1,
                          w - (int)(8 * scale), lh};
        if (ui_hit(line)) start_sel = i;
        if (i == start_sel)
            DrawRectangleRec(line, MRED);
        int lw = ui_measure(label, fs);
        ui_text(label, x0 + (w - lw) / 2, y, fs,
                i == start_sel ? MWHITE : (Color){136, 136, 136, 255});
        if (i == start_sel && ui_pressed()) {
            if (i < COURSE_COUNT) sel_request = i + 1;
            else if (i == COURSE_COUNT) sel_request = -1;
            else sel_request = 1 + (int)(wx_rand() * COURSE_COUNT) % COURSE_COUNT;
            start_menu = 0;
        }
    }
}

/* Set by page_game when the panel's DEBUG button is clicked. */
static int game_debug_clicked;
/* --bezel: the play front end's surround on the game page, so `make play`
 * and `make debug` are the same window and the debug pages are a button
 * away rather than a different program. */
static int use_bezel;
static int offline_mode;
int native_overrides_count(void);

static void page_game(void)
{
    if (use_bezel) {
        /* The bezel is laid out in the logical canvas, and the mouse has
         * to be mapped into it too or the button is only clickable where
         * it is drawn on a 1:1 window. */
        Bezel bz = bezel_begin(4.0f, 3.0f,
                               (Rectangle){0, 0, WIN_W, WIN_H});
        draw_game_picture(bz.game);
        DrawRectangleLinesEx((Rectangle){bz.game.x - 2, bz.game.y - 2,
                                         bz.game.width + 4,
                                         bz.game.height + 4},
                             2, BTN_EDGE);
        /* A race gives the map something to show; the menus do not. */
        int mapping = 0;
        Rectangle mr = {0};
        course_read_live = 1;
        if (!offline_mode && course_data_ready()) {
            float mw = bz.right.width - 16;
            mr = (Rectangle){bz.right.x + 8, bz.right.y + 8, mw, mw};
            mapping = 1;
            course_integrate();
        }
        if (bezel_panels(&bz, GetFPS(), native_overrides_count(), 1,
                         mpos(), mapping ? (int)mr.height + 14 : 0))
            game_debug_clicked = 1;
        if (mapping) draw_race_map(mr);
        course_read_live = 0;
        start_menu_draw(bz.game);
        return;
    }
    float s = (float)WIN_W / SCREEN_W;
    if ((float)(WIN_H - 26) / SCREEN_H < s) s = (float)(WIN_H - 26) / SCREEN_H;
    Rectangle src = { 0, 0, SCREEN_W, SCREEN_H };
    Rectangle dst = { (WIN_W - SCREEN_W * s) / 2,
                      26 + (WIN_H - 26 - SCREEN_H * s) / 2,
                      SCREEN_W * s, SCREEN_H * s };
    DrawTexturePro(game_screen, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
    ui_text("1 GAME   2 COURSES   F5 freeze   F11 full   P pause   "
            "|   pad SELECT pages", 16, 4, 18, LABEL);
}

int main(int argc, char **argv)
{
    WhdConfig whd = { .dir = "original/Lotus2CD32",
                      .slave = "Lotus2CD32.slave" };
    /* Auto-fire is OFF unless asked for.  It used to default to "press
     * fire every 100 frames from frame 2100", which is how the capture
     * runs drive themselves into a race -- and it does not stop at the
     * race.  On a menu it keeps choosing things: pick VIEW EXTRAS, wait,
     * and the harness starts a game for you.  Anything that needs it
     * passes --fire-from explicitly. */
    long fire_from = -1, fire_period = 100, shot_at = -1;
    const char *shot_path = NULL;
    int mode = MODE_GAME, shot_mode = MODE_GAME;
    const char *course_path = "re/pipeline/course_forest.l2c";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) whd.dir = argv[++i];
        else if (!strcmp(argv[i], "--fire-from") && i + 1 < argc)
            fire_from = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fire-period") && i + 1 < argc)
            fire_period = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--shot") && i + 2 < argc) {
            shot_at = strtol(argv[++i], NULL, 0);
            shot_path = argv[++i];
        }
        else if (!strcmp(argv[i], "--course") && i + 1 < argc)
            course_path = argv[++i];
        else if (!strcmp(argv[i], "--live")) course_path = NULL;
        else if (!strcmp(argv[i], "--bezel")) use_bezel = 1;
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) {
            const char *w = argv[++i];
            for (int k = 0; k < COURSE_COUNT; k++)
                if (!strcasecmp(w, COURSES[k].name)) course_sel = k;
        }
        else if (!strcmp(argv[i], "--static")) course_follow = 0;
        else if (!strcmp(argv[i], "--roadpc") && i + 1 < argc)
            road_capture_pc = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--scrub") && i + 1 < argc) {
            course_scrub = strtof(argv[++i], NULL);
            course_follow = 0;
            course_drive = 1;
        }
        else if (!strcmp(argv[i], "--page") && i + 1 < argc) {
            const char *w = argv[++i];
            shot_mode = !strcmp(w, "COURSE") ? MODE_COURSE
                      : !strcmp(w, "GFX")    ? MODE_GFX
                      : !strcmp(w, "SOUND")  ? MODE_SOUND : MODE_GAME;
            /* opening ON a debug page means the game is NOT running when
             * you get there -- the guest only runs on the game page --
             * so `make debug` no longer starts a race behind your back */
            mode = shot_mode;
        }
        else {
            fprintf(stderr,
                "usage: lotus2_view [--dir DIR] [--fire-from N]\n"
                "       [--fire-period N] [--page NAME] [--shot FRAME PATH]\n");
            return 2;
        }
    }

    SetTraceLogLevel(LOG_WARNING);
    /* Resizable is safe now.  It was off because a tiling window manager
     * took it as licence to hand out a portrait slot, and the layout of
     * the day could not cope; everything is drawn into a fixed logical
     * canvas and blitted scaled and centred, so any window shape works
     * and the bezel keeps its 16:9 inside whatever it is given. */
    /* ...except in --shot mode, where the window manager choosing the
     * size makes the captures non-deterministic, which is the one thing
     * a capture must not be. */
    if (shot_at < 0) SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(WIN_W, WIN_H, "Lotus 2 - native RE viewer");
    SetWindowMinSize(800, 450);
    SetExitKey(KEY_NULL);
    /* The logical canvas is 1920x1080 and gets blitted scaled to fit, but
     * the WINDOW opened at that size regardless -- so on any desktop
     * smaller than 1080 rows of work area the bottom of it, control bar
     * included, was off the screen.  Fit the monitor, then centre. */
    {
        int mon = GetCurrentMonitor();
        int mw = GetMonitorWidth(mon), mh = GetMonitorHeight(mon);
        if (mw > 320 && mh > 240) {
            int aw = mw - 80, ah = mh - 120, ww = WIN_W, wh = WIN_H;
            if (ww > aw || wh > ah) {
                float k = (float)aw / ww;
                if ((float)ah / wh < k) k = (float)ah / wh;
                ww = (int)(ww * k);
                wh = (int)(wh * k);
                SetWindowSize(ww, wh);
            }
            Vector2 mp = GetMonitorPosition(mon);
            SetWindowPosition((int)mp.x + (mw - ww) / 2,
                              (int)mp.y + (mh - wh) / 2);
        }
    }
    /* Run flat out while booting into a race; the tool only needs 50 Hz
     * once the course is loaded and the emulation is frozen. */
    /* Flat out while booting into a race, which is what the debug pages
     * want.  As the play front end it has to run at the machine's rate
     * instead, or the game is simply too fast to drive. */
    SetTargetFPS(use_bezel ? 50 : 0);
    canvas_rt = LoadRenderTexture(WIN_W, WIN_H);
    /* somewhere for the GRAPHICS page's EXPORT to land; gitignored,
     * because what it saves is the retail game's art */
    if (!DirectoryExists("graphics")) MakeDirectory("graphics");
    ui_font = GetFontDefault();
    static const char *fonts[] = {
        "assets/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf", NULL };
    for (int i = 0; fonts[i]; i++)
        if (FileExists(fonts[i])) {
            ui_font = LoadFontEx(fonts[i], 40, NULL, 0);
            SetTextureFilter(ui_font.texture, TEXTURE_FILTER_BILINEAR);
            ui_font_ok = 1;
            break;
        }

    int offline = course_path && course_load(course_path);
    offline_mode = offline;
    AudioStream stream = {0};
    if (!offline) {
        InitAudioDevice();
        SetAudioStreamBufferSizeDefault(FRAME_SAMPLES);
        stream = LoadAudioStream(AUDIO_RATE, 16, 2);
        SetAudioStreamCallback(stream, audio_pull_cb);
        PlayAudioStream(stream);
        amiga_audio_generate(FRAME_SAMPLES * 4);   /* prime the ring */
    }
    if (!offline) {
        amiga_init();
        if (!whdload_boot(&whd)) {
            fprintf(stderr, "lotus2_view: boot failed\n");
            return 1;
        }
        amiga_enable_video(true);
    }
    if (shot_at >= 0) { mode = shot_mode; ui_input = 0; }

    Image image = { .data = framebuf, .width = SCREEN_W, .height = SCREEN_H,
                    .mipmaps = 1,
                    .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    Texture2D screen = LoadTextureFromImage(image);
    game_screen = screen;
    Image rimg = { .data = road_img, .width = VIEW_W, .height = VIEW_H,
                   .mipmaps = 1,
                   .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    road_tex = LoadTextureFromImage(rimg);
    Image gimg = { .data = gfx_img, .width = GFX_MAX_W, .height = GFX_MAX_H,
                   .mipmaps = 1,
                   .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    gfx_tex = LoadTextureFromImage(gimg);
    amiga_set_pc_hook(road_pc_hook);
    bool paused = false, frozen = false, freeze_on_course = false;

    bezel_scaler = UP_NAME[up_mode];
    if (offline) { frozen = true; SetTargetFPS(50); }
    long draws = 0;
    while (!WindowShouldClose() && !(!offline && amiga_stopped())) {
        /* --static freezes the game, so in shot mode only switch it on
         * once the run has reached the capture frame. */
        if (IsKeyPressed(KEY_P)) paused = !paused;
        if (IsKeyPressed(KEY_ESCAPE)) {
            if (start_menu) start_menu = 0;
            else mode = MODE_GAME;
        }
        if (IsKeyPressed(KEY_F5)) { frozen = !frozen; freeze_on_course = false; }
        if (IsKeyPressed(KEY_F11)) ToggleFullscreen();
        if (IsKeyPressed(KEY_ONE))   mode = MODE_GAME;
        if (IsKeyPressed(KEY_TWO))   mode = MODE_COURSE;
        if (sel_request) {
            int want = sel_request;
            sel_request = 0;
            sel_series = (want < 0);
            int course = sel_series ? 0 : want - 1;
            if (sel_start(course, &whd)) mode = MODE_GAME;
        }
        /* the series: when a course's replay has handed over and the
         * next one is asked for, it starts from the top of the boot */
        if (IsKeyPressed(KEY_F4)) road_extras = !road_extras;
        if (IsKeyPressed(KEY_F6)) car_hue = (car_hue + 1) % CAR_HUE_COUNT;
        if (IsKeyPressed(KEY_F3)) {
            up_mode = (up_mode + 1) % UP_COUNT;
            bezel_scaler = UP_NAME[up_mode];
        }
        if (IsKeyPressed(KEY_THREE)) mode = MODE_GFX;
        if (IsKeyPressed(KEY_FOUR))  mode = MODE_SOUND;
        /* D from the play screen, or the panel's DEBUG button */
        /* Consume the click.  It is a LATCH set during the draw phase,
         * and leaving it set meant ESC put you back on the game page and
         * the stale latch sent you straight out again on the same frame
         * -- there was no way back. */
        int want_debug = game_debug_clicked;
        game_debug_clicked = 0;

        /* X TOGGLES the preview, from the keyboard or the pad.
         *
         * The pad half has to go through js_button_down, not raylib's
         * gamepad API: pad.c opens /dev/input/js0 directly and reads the
         * events itself, so raylib's own gamepad may never see the
         * device at all -- which is why pressing X on the pad did
         * nothing while the log showed the button arriving.  Button 2 is
         * X on an SDL-style layout (0 A, 1 B, 2 X, 3 Y). */
        int pad_x_edge = js_edge(0, 2);

        if (IsKeyPressed(KEY_X) || IsKeyPressed(KEY_D) || pad_x_edge ||
            want_debug)
            mode = (mode == MODE_GAME) ? MODE_COURSE : MODE_GAME;
        /* the pad's shoulder buttons walk the debug pages once you are
         * in them; on the game page they belong to the game */
        if (mode != MODE_GAME) {
            if (js_edge(0, 5)) mode = mode + 1 > MODE_SOUND ? MODE_COURSE
                                                           : mode + 1;
            if (js_edge(0, 4)) mode = mode - 1 < MODE_COURSE ? MODE_SOUND
                                                            : mode - 1;
        }
        if (IsKeyPressed(KEY_F2)) {
            Image s = LoadImageFromScreen();
            ExportImage(s, "lotus2view.png");
            UnloadImage(s);
        }

        /* GRAPHICS and SOUND are windows on LIVE state: the screen the
         * copper is showing, the samples the voices are playing.  Frozen
         * they show an instant, and if you reach them before the game
         * has drawn or played anything they show nothing at all -- which
         * is what "not populated" was.  So the guest keeps running on
         * those.
         *
         * COURSE is the exception, and the reason the freeze exists: it
         * is a static preview off a snapshot, and leaving the game
         * running behind it meant walking away and coming back to a race
         * in progress.
         *
         * Either way the pad and keyboard reach the guest only on the
         * game page, so nothing steers it while you are looking. */
        if (!offline && !paused && !frozen && mode != MODE_COURSE) {
            js_poll();
            /* Only the game page steers the game.  The debug pages use
             * the same arrows, stick and buttons to scrub and zoom, and
             * those were reaching the guest as well -- so scrubbing the
             * course on the COURSE page was also working the menu behind
             * it, and starting a race. */
            /* TWO PLAYERS, both here at the machine.  The game reads
             * port 1 for the near car and port 0 for the far one -- and
             * port 0 is also what the menus read.  So port 0 carries
             * player 2 once there IS a player 2, and player 1 until
             * then, or nobody could work the menus on their own.
             *
             * "Is there a player 2" is answered by them doing something:
             * a second pad appearing, or the player 2 keys being
             * pressed.  It latches, because a player who lets go of the
             * controls has not left. */
            uint8_t p1 = 0, p2 = 0;
            static int p2_present;
            if (sel_running) {
                uint8_t b = sel_frame();
                joy_state[0] = joy_state[1] = b;
                amiga_run_frame();
                {
                    const int target = FRAME_SAMPLES * 4;
                    int fill = amiga_audio_fill();
                    if (fill < target) amiga_audio_generate(target - fill);
                }
                goto drawn;
            }
            if (mode == MODE_GAME) {
                p1 = keyboard_stick(true) | gamepad_stick(0);
                /* fire on the title asks which course; during a race it
                 * is the accelerator and must go straight through */
                static int fire_was;
                int fire = (p1 & 0x10) != 0;
                /* course_data_ready() goes through course_byte(), which
                 * prefers the PREVIEW's snapshot when one is loaded --
                 * so without this it always said "a race is up" and the
                 * menu never appeared. */
                course_read_live = 1;
                int racing = course_data_ready();
                course_read_live = 0;
                /* The FIRST fire is the one that leaves the title, and
                 * the menu belongs on the setup screen rather than in
                 * front of it -- so that one goes through and the next
                 * one asks. */
                static int fires;
                if (fire && !fire_was && !racing && ++fires > 1 && !start_menu)
                    start_menu = 1;
                if (racing) fires = 0;
                fire_was = fire;
                if (start_menu) { start_menu_input(p1); p1 = 0; }
                p2 = keyboard_stick(false) | gamepad_stick(1);
                if (p2 || js_present(1)) p2_present = 1;
            }
            uint8_t stick = p1;
            if (fire_from >= 0 && swiv_frame_no >= fire_from)
                stick |= fire_period
                    ? (((swiv_frame_no / fire_period) % 2) ? 0x10 : 0x00)
                    : 0x10;
            joy_state[1] = stick;                     /* near car */
            joy_state[0] = p2_present ? p2 : stick;   /* far car, or menus */
            (void)0;
            amiga_run_frame();
            {   /* keep ~80 ms of slack in the ring for the callback */
                const int target = FRAME_SAMPLES * 4;
                int fill = amiga_audio_fill();
                if (fill < target) amiga_audio_generate(target - fill);
            }
        drawn: ;
            /* The course landing used to freeze the emulation here.  It
             * is a debug convenience, not a default: F5 freezes when you
             * want to read state, and the game keeps running otherwise. */
            if (course_data_ready() && freeze_on_course) {
                frozen = true;
                SetTargetFPS(50);
                fprintf(stderr, "lotus2_view: course loaded at frame %ld, "
                                "emulation frozen\n", swiv_frame_no);
            }
        }
        UpdateTexture(screen, framebuf);

        /* --- draw the logical canvas --- */
        /* The pages get the whole window.  Squeezing one into the gap
         * between the bezel panels shrank every control to fit a third
         * of the width; the bezel is the GAME's surround, not a frame to
         * hang tools inside. */
        BeginTextureMode(canvas_rt);
        ClearBackground(BAR_BG);
        switch (mode) {
        case MODE_COURSE:  page_course(); break;
        case MODE_GFX:     page_gfx(); break;
        case MODE_SOUND:   page_sound(); break;
        default:           page_game(); break;
        }

        /* --- control bar ---
         * The play screen is the game and its surround, nothing else:
         * no bar, no frame counter, no PC.  The bar belongs to the
         * debug pages, and its only job there is to get you back. */
        if (mode != MODE_GAME) {
            int by = WIN_H - BAR_H;
            DrawRectangle(0, by, WIN_W, BAR_H, BAR_BG);
            float r1 = by + 26, r2 = by + 72, bh = 40;
            if (button((Rectangle){8, r1, 220, bh}, "BACK TO GAME", 0))
                mode = MODE_GAME;
            if (button((Rectangle){236, r1, 150, bh}, "COURSES",
                       mode == MODE_COURSE)) mode = MODE_COURSE;
            if (button((Rectangle){394, r1, 150, bh}, "GRAPHICS",
                       mode == MODE_GFX)) mode = MODE_GFX;
            if (button((Rectangle){552, r1, 130, bh}, "SOUND",
                       mode == MODE_SOUND)) mode = MODE_SOUND;
            ui_text("X or ESC back to the game   2 courses  3 graphics  "
                    "4 sound   pad shoulders page   F2 screenshot",
                    8, (int)r2 + 12, 18, LIGHTGRAY);
        }
        EndTextureMode();

        /* --- blit the canvas, scaled and centred --- */
        int sw = GetScreenWidth(), sh = GetScreenHeight();
        float s1 = sw / (float)WIN_W, s2 = sh / (float)WIN_H;
        view_scale = s1 < s2 ? s1 : s2;
        view_ox = (sw - WIN_W * view_scale) * 0.5f;
        view_oy = (sh - WIN_H * view_scale) * 0.5f;
        BeginDrawing();
        ClearBackground(BLACK);
        DrawTexturePro(canvas_rt.texture,
                       (Rectangle){0, 0, WIN_W, -WIN_H},
                       (Rectangle){view_ox, view_oy, WIN_W * view_scale,
                                   WIN_H * view_scale},
                       (Vector2){0, 0}, 0.0f, WHITE);
        EndDrawing();

        draws++;
        /* let a few frames render before grabbing: the first backbuffer is
         * not complete when the window has only just appeared */
        if (shot_at >= 0 && draws > 90 &&
            (frozen || swiv_frame_no >= shot_at)) {
            /* TakeScreenshot() prepends the working directory even to an
             * absolute path, so grab and export the image ourselves. */
            Image s = LoadImageFromScreen();
            bool ok = ExportImage(s, shot_path);
            UnloadImage(s);
            fprintf(stderr, "lotus2_view: %s %s at frame %ld\n",
                    ok ? "wrote" : "FAILED to write", shot_path,
                    swiv_frame_no);
            break;
        }
    }

    if (stream.buffer) {
        UnloadAudioStream(stream);
        CloseAudioDevice();
    }
    UnloadTexture(screen);
    UnloadTexture(road_tex);
    UnloadTexture(gfx_tex);
    UnloadRenderTexture(canvas_rt);
    CloseWindow();
    return 0;
}
