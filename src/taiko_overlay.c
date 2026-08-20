/* See taiko_overlay.h.
 *
 * The styling follows TaikoZucchini's core/title_render.c (MIT, same author):
 * white fill, and the outline built by disk-dilating the glyph's own coverage
 * mask rather than by stroking the outline, which is what gives the title's
 * text its rounded, even border. Only the parts a six-digit code needs are
 * here -- no title cache, no vertical text, no per-song prerender.
 */
#include "taiko_overlay.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "taiko_pairing_pill.h"

/* The artwork sets the layout: a red disc on the left for the countdown, a
 * yellow body for the code. Both texts are black, as on the cabinet. */
enum {
    OVERLAY_WIDTH = TAIKO_PAIRING_PILL_WIDTH,
    OVERLAY_HEIGHT = TAIKO_PAIRING_PILL_HEIGHT,
    PILL_DISC_WIDTH = 64,             /* the red disc at the left end */
    CODE_CENTER_X = 158,
    CODE_PIXELS = 34,
    COUNTDOWN_PIXELS = 26,
};

static const uint32_t COLOR_TEXT = 0xFF000000u;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_pixels[OVERLAY_WIDTH * OVERLAY_HEIGHT];
static uint32_t g_version;
static int      g_visible;
static char     g_code[16];
static long     g_deadline;
static int      g_drawn_remaining = -1;

static FT_Library g_library;
static FT_Face    g_face;
static int        g_font_state;    /* 0 untried, 1 ready, -1 unavailable */

/* Published to the RSX backend, which blits whatever this returns over the
 * frame. Weak so a build with a backend that has no overlay support still
 * links; the assignment below is then simply skipped. */
extern const uint32_t* (*g_rsx_overlay_frame)(int* width, int* height,
                                              uint32_t* version)
    __attribute__((weak));

__attribute__((constructor))
static void taiko_overlay_register(void)
{
    if (&g_rsx_overlay_frame) g_rsx_overlay_frame = taiko_overlay_frame;
}

static long monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static int font_ready(void)
{
    if (g_font_state) return g_font_state > 0;
    g_font_state = -1;

    const char* path = getenv("TAIKO_OVERLAY_FONT");
    if (!path || !path[0]) path = "fonts/font.ttf";
    if (FT_Init_FreeType(&g_library) != 0) return 0;
    if (FT_New_Face(g_library, path, 0, &g_face) != 0) {
        fprintf(stderr, "[taiko_overlay] no font at '%s'; "
                        "set TAIKO_OVERLAY_FONT to the game font\n", path);
        return 0;
    }
    g_font_state = 1;
    fprintf(stderr, "[taiko_overlay] font '%s' (%s)\n", path,
            g_face->family_name ? g_face->family_name : "?");
    return 1;
}

/* --------------------------------------------------------------------------
 * Drawing
 * -----------------------------------------------------------------------*/

/* Alpha-composite one coverage sample. The overlay keeps its own alpha so the
 * pill's rounded ends stay transparent on screen. */
static void put_pixel(int x, int y, uint32_t colour, unsigned coverage)
{
    if (x < 0 || y < 0 || x >= OVERLAY_WIDTH || y >= OVERLAY_HEIGHT || !coverage)
        return;

    uint32_t* target = &g_pixels[(size_t)y * OVERLAY_WIDTH + x];
    const unsigned source_alpha = ((colour >> 24) & 0xFF) * coverage / 255u;
    if (!source_alpha) return;

    const unsigned destination_alpha = (*target >> 24) & 0xFF;
    const unsigned destination_weight =
        (destination_alpha * (255u - source_alpha) + 127u) / 255u;
    const unsigned out_alpha = source_alpha + destination_weight;
    if (!out_alpha) return;

    uint32_t out = out_alpha << 24;
    for (int shift = 0; shift < 24; shift += 8) {
        const unsigned src = (colour >> shift) & 0xFF;
        const unsigned dst = (*target >> shift) & 0xFF;
        out |= (((src * source_alpha + dst * destination_weight + out_alpha / 2u) /
                 out_alpha) & 0xFFu) << shift;
    }
    *target = out;
}

static void draw_pill(void)
{
    for (int i = 0; i < OVERLAY_WIDTH * OVERLAY_HEIGHT; i++) {
        const unsigned char* pixel = &taiko_pairing_pill_rgba[i * 4];
        g_pixels[i] = ((uint32_t)pixel[3] << 24) | ((uint32_t)pixel[2] << 16) |
                      ((uint32_t)pixel[1] << 8) | (uint32_t)pixel[0];
    }
}

static void draw_glyph(const FT_Bitmap* bitmap, int origin_x, int origin_y)
{
    for (unsigned row = 0; row < bitmap->rows; row++)
        for (unsigned column = 0; column < bitmap->width; column++)
            put_pixel(origin_x + (int)column, origin_y + (int)row, COLOR_TEXT,
                      bitmap->buffer[row * (unsigned)bitmap->pitch + column]);
}

static int text_width(const char* text, int pixels)
{
    int width = 0;
    if (FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)pixels) != 0) return 0;
    for (const char* c = text; *c; c++) {
        if (FT_Load_Char(g_face, (FT_ULong)(unsigned char)*c, FT_LOAD_DEFAULT) != 0)
            continue;
        width += (int)(g_face->glyph->advance.x >> 6);
    }
    return width;
}

/* Centred on `centre_x`, and vertically centred on the pill rather than sat on
 * a baseline: the strings here are digits and a hyphen, so their ink box is
 * what should look centred. */
static void draw_text(const char* text, int pixels, int centre_x)
{
    if (FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)pixels) != 0) return;

    int top = OVERLAY_HEIGHT, bottom = 0;
    for (const char* c = text; *c; c++) {
        if (FT_Load_Char(g_face, (FT_ULong)(unsigned char)*c, FT_LOAD_DEFAULT) != 0)
            continue;
        const FT_Glyph_Metrics* metrics = &g_face->glyph->metrics;
        const int glyph_top = (int)(metrics->horiBearingY >> 6);
        const int glyph_bottom = glyph_top - (int)(metrics->height >> 6);
        if (glyph_top > bottom) bottom = glyph_top;
        if (glyph_bottom < top) top = glyph_bottom;
    }
    const int baseline = (OVERLAY_HEIGHT + bottom + top) / 2;

    int pen_x = centre_x - text_width(text, pixels) / 2;
    for (const char* c = text; *c; c++) {
        if (FT_Load_Char(g_face, (FT_ULong)(unsigned char)*c, FT_LOAD_RENDER) != 0)
            continue;
        const FT_GlyphSlot glyph = g_face->glyph;
        draw_glyph(&glyph->bitmap, pen_x + glyph->bitmap_left,
                   baseline - glyph->bitmap_top);
        pen_x += (int)(glyph->advance.x >> 6);
    }
}

static void render(int remaining)
{
    char code[16];
    char countdown[4];

    draw_pill();

    /* 661722 reads as 661-722 on the cabinet. */
    if (strlen(g_code) == 6)
        snprintf(code, sizeof(code), "%.3s-%s", g_code, g_code + 3);
    else
        snprintf(code, sizeof(code), "%s", g_code);
    draw_text(code, CODE_PIXELS, CODE_CENTER_X);

    snprintf(countdown, sizeof(countdown), "%d", remaining > 99 ? 99 : remaining);
    draw_text(countdown, COUNTDOWN_PIXELS, PILL_DISC_WIDTH / 2);

    g_drawn_remaining = remaining;
    ++g_version;
}

/* --------------------------------------------------------------------------
 * Public entry points
 * -----------------------------------------------------------------------*/

void taiko_overlay_set_pairing(const char* code, int expires_in)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_code, sizeof(g_code), "%s", code ? code : "");
    g_deadline = monotonic_seconds() + (expires_in > 0 ? expires_in : 0);
    g_visible = g_code[0] != '\0';
    g_drawn_remaining = -1;
    pthread_mutex_unlock(&g_lock);
}

void taiko_overlay_clear(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_visible) ++g_version;
    g_visible = 0;
    g_code[0] = '\0';
    pthread_mutex_unlock(&g_lock);
}

const uint32_t* taiko_overlay_frame(int* width, int* height, uint32_t* version)
{
    pthread_mutex_lock(&g_lock);

    int remaining = (int)(g_deadline - monotonic_seconds());
    if (remaining < 0) remaining = 0;
    if (g_visible && remaining == 0) {
        g_visible = 0;
        ++g_version;
    }
    if (!g_visible || !font_ready()) {
        const uint32_t current = g_version;
        pthread_mutex_unlock(&g_lock);
        if (version) *version = current;
        return NULL;
    }
    if (remaining != g_drawn_remaining) render(remaining);

    if (width) *width = OVERLAY_WIDTH;
    if (height) *height = OVERLAY_HEIGHT;
    if (version) *version = g_version;
    pthread_mutex_unlock(&g_lock);
    return g_pixels;
}
