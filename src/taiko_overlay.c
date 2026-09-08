/* See taiko_overlay.h.
 *
 * The styling follows TaikoZucchini's core/title_render.c (MIT, same author):
 * white fill, and the outline built by disk-dilating the glyph's own coverage
 * mask rather than by stroking the outline, which is what gives the title's
 * text its rounded, even border. A bounded text-run cache retains metrics
 * and transparent outlined bitmaps while browser rows animate.
 */
#include "taiko_overlay.h"
#include "rsx_host_ui.h"

#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_STROKER_H

#include "taiko_pairing_pill.h"

/* The artwork sets the layout: a red disc on the left for the countdown, a
 * yellow body for the code. Both texts are black, as on the cabinet. */
enum {
    PILL_WIDTH = TAIKO_PAIRING_PILL_WIDTH,
    PILL_HEIGHT = TAIKO_PAIRING_PILL_HEIGHT,
    HOST_WIDTH = 1280,
    HOST_HEIGHT = 720,
    OVERLAY_MAX_WIDTH = HOST_WIDTH,
    OVERLAY_MAX_HEIGHT = HOST_HEIGHT,
    PILL_DISC_WIDTH = 64,             /* the red disc at the left end */
    CODE_CENTER_X = 158,
    CODE_PIXELS = 34,
    COUNTDOWN_PIXELS = 26,
};

static const uint32_t COLOR_TEXT = 0xFFFFFFFFu;      /* fill */
static const uint32_t COLOR_TEXT_OUTLINE = 0xFF000000u;
#define RGB_COLOUR(red, green, blue) \
    (0xFF000000u | ((uint32_t)(blue) << 16) | \
     ((uint32_t)(green) << 8) | (uint32_t)(red))
enum { TEXT_OUTLINE_RADIUS = 3 };
static int g_outline_radius = TEXT_OUTLINE_RADIUS;
static HostUiEmit g_ui_emit;
static void* g_ui_user;
static float g_ui_scale = 1.0f;
static int visit_host_ui(float scale, HostUiEmit emit, void* user, HostUiInfo* info);
extern HostUiVisit g_rsx_host_ui_visit __attribute__((weak));

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_frame_pixels[OVERLAY_MAX_WIDTH * OVERLAY_MAX_HEIGHT];
static uint32_t* g_pixels = g_frame_pixels;
static uint32_t g_version;
static int      g_visible;
static int      g_mode;             /* 1 pairing, 2 status, 3--5 host screens */
static int      g_width = PILL_WIDTH;
static int      g_height = PILL_HEIGHT;
static int      g_host_selection;
static char     g_code[16];
static char     g_status[32];
static char     g_player_name[64];
static char     g_song_id[32];
static char     g_song_title[256];
static char     g_song_genre[128];
static char     g_song_difficulty[32];
static char     g_song_query[128];
static char     g_browser_save_status[96];
static uint32_t g_song_unique_id;
static unsigned g_song_index;
static unsigned g_song_match_total;
static unsigned g_song_catalog_total;
static char     g_song_category[64];
static unsigned g_song_category_index;
static unsigned g_song_category_total;
static uint8_t  g_song_difficulty_mask;
static int g_browser_players_enabled;
static uint8_t g_browser_joined, g_browser_ready;
static uint8_t g_browser_difficulties[2];
static int      g_song_search_active;
static int      g_song_browser_level;
static int      g_song_selection_is_exit;
typedef struct song_row_storage {
    char title[256];
    char genre[128];
    unsigned catalog_index;
    int selected;
    int kind;
    unsigned difficulty, stars;
    uint8_t cursors, ready;
    float from_y, from_x;
} song_row_storage;
static song_row_storage g_song_rows[TAIKO_OVERLAY_SONG_ROW_COUNT];
static unsigned g_song_row_count;
static double g_song_animation_start, g_song_last_render;
static int g_song_animating;
static int g_gpu_animation_pending;
/* Three cached, opaque panels keep overlapping text/cards fading as one layer.
 * Only the short handoff uses these 1280x720 snapshots; normal UI stays native
 * resolution. No per-frame text rasterization or GPU texture uploads. */
static uint32_t g_handoff_pixels[HOST_WIDTH * HOST_HEIGHT];
static int g_handoff; /* 1 entering, -1 leaving */
static double g_handoff_start;
static int g_handoff_snapshot;
static uint64_t g_handoff_ids[3];
static const double HANDOFF_MS = 320.0;
static long     g_deadline;
static int      g_drawn_remaining = -1;

static FT_Library g_library;
static FT_Face    g_face;
static int        g_font_state;    /* 0 untried, 1 ready, -1 unavailable */

/* Published to the RSX backend. Weak symbols keep null/alternate renderer
 * builds independent of this title extension. */
extern RsxHostFrameCopy g_rsx_host_frame_copy __attribute__((weak));
extern void rsx_sdl_gpu_backend_wake(void) __attribute__((weak));

__attribute__((constructor))
static void taiko_overlay_register(void)
{
    if (&g_rsx_host_ui_visit) g_rsx_host_ui_visit = visit_host_ui;
    if (&g_rsx_host_frame_copy)
        g_rsx_host_frame_copy = taiko_host_frame_copy;
}

static void wake_renderer(void)
{
    if (rsx_sdl_gpu_backend_wake) rsx_sdl_gpu_backend_wake();
}

static long monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static double monotonic_milliseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static float song_ease(void)
{
    double t = (monotonic_milliseconds() - g_song_animation_start) / 180.0;
    if (t >= 1.0) return 1.0f;
    if (t < 0.0) t = 0.0;
    double inverse = 1.0 - t;
    return (float)(1.0 - inverse * inverse * inverse * inverse * inverse);
}

static int row_target_x(int kind, int selected)
{
    return kind == TAIKO_OVERLAY_ROW_DIFFICULTY ? (selected ? 652 : 674)
                                               : (selected ? 594 : 628);
}

#ifdef TAIKO_OVERLAY_FONT_EMBEDDED
/* Generated by tools/embed_font.py at configure time: the digits and hyphen of
 * the game font, so the executable needs no font file beside it. */
extern const unsigned char taiko_overlay_font_data[];
extern const unsigned taiko_overlay_font_size;
#endif

static int font_ready(void)
{
    if (g_font_state) return g_font_state > 0;
    g_font_state = -1;

    if (FT_Init_FreeType(&g_library) != 0) return 0;

    /* An explicit TAIKO_OVERLAY_FONT always wins, so a different face can be
     * tried without rebuilding. */
    const char* path = getenv("TAIKO_OVERLAY_FONT");
#ifdef TAIKO_OVERLAY_FONT_EMBEDDED
    if (!path || !path[0]) {
        if (FT_New_Memory_Face(g_library, taiko_overlay_font_data,
                               (FT_Long)taiko_overlay_font_size, 0,
                               &g_face) != 0) {
            fprintf(stderr, "[taiko_overlay] embedded font is unusable\n");
            return 0;
        }
        g_font_state = 1;
        fprintf(stderr, "[taiko_overlay] font embedded, %u bytes (%s)\n",
                taiko_overlay_font_size,
                g_face->family_name ? g_face->family_name : "?");
        return 1;
    }
#endif
    if (!path || !path[0]) path = "fonts/font.ttf";
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
    if (x < 0 || y < 0 || x >= g_width || y >= g_height || !coverage)
        return;

    uint32_t* target = &g_pixels[(size_t)y * g_width + x];
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
    g_width = PILL_WIDTH;
    g_height = PILL_HEIGHT;
    for (int i = 0; i < PILL_WIDTH * PILL_HEIGHT; i++) {
        const unsigned char* pixel = &taiko_pairing_pill_rgba[i * 4];
        g_pixels[i] = ((uint32_t)pixel[3] << 24) | ((uint32_t)pixel[2] << 16) |
                      ((uint32_t)pixel[1] << 8) | (uint32_t)pixel[0];
    }
}

/* The outline is the glyph's own coverage grown into a disc, which is what
 * keeps the border even around the font's rounded strokes. */
static void draw_glyph(const FT_Bitmap* bitmap, int origin_x, int origin_y,
                       int outline)
{
    for (unsigned row = 0; row < bitmap->rows; row++) {
        for (unsigned column = 0; column < bitmap->width; column++) {
            const unsigned coverage =
                bitmap->buffer[row * (unsigned)bitmap->pitch + column];
            if (!coverage) continue;
            const int px = origin_x + (int)column;
            const int py = origin_y + (int)row;
            if (!outline) {
                put_pixel(px, py, COLOR_TEXT, coverage);
                continue;
            }
            for (int dy = -g_outline_radius; dy <= g_outline_radius; dy++)
                for (int dx = -g_outline_radius; dx <= g_outline_radius; dx++)
                    if (dx * dx + dy * dy <= g_outline_radius * g_outline_radius)
                        put_pixel(px + dx, py + dy, COLOR_TEXT_OUTLINE, coverage);
        }
    }
}

static FT_ULong text_codepoint(const unsigned char** source)
{
    const unsigned char* cursor = *source;
    FT_ULong value = *cursor++;
    unsigned remaining = 0;
    if ((value & 0xE0u) == 0xC0u) { value &= 0x1Fu; remaining = 1; }
    else if ((value & 0xF0u) == 0xE0u) { value &= 0x0Fu; remaining = 2; }
    else if ((value & 0xF8u) == 0xF0u) { value &= 0x07u; remaining = 3; }
    while (remaining--) {
        if ((*cursor & 0xC0u) != 0x80u) { value = 0xFFFDu; break; }
        value = (value << 6) | (*cursor++ & 0x3Fu);
    }
    *source = cursor;
    return value;
}

static int text_width_uncached(const char* text, int pixels)
{
    int width = 0;
    if (FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)pixels) != 0) return 0;
    const unsigned char* cursor = (const unsigned char*)text;
    while (*cursor) {
        FT_ULong codepoint = text_codepoint(&cursor);
        if (FT_Load_Char(g_face, codepoint, FT_LOAD_DEFAULT) != 0)
            continue;
        width += (int)(g_face->glyph->advance.x >> 6);
    }
    return width;
}

/* Centred on `centre_x`, and vertically centred on the pill rather than sat on
 * a baseline: the strings here are digits and a hyphen, so their ink box is
 * what should look centred. */
static void draw_text_uncached(const char* text, int pixels, int centre_x, int centre_y)
{
    if (FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)pixels) != 0) return;

    int top = INT_MAX, bottom = 0;
    const unsigned char* cursor = (const unsigned char*)text;
    while (*cursor) {
        FT_ULong codepoint = text_codepoint(&cursor);
        if (FT_Load_Char(g_face, codepoint, FT_LOAD_DEFAULT) != 0)
            continue;
        const FT_Glyph_Metrics* metrics = &g_face->glyph->metrics;
        const int glyph_top = (int)(metrics->horiBearingY >> 6);
        const int glyph_bottom = glyph_top - (int)(metrics->height >> 6);
        if (glyph_top > bottom) bottom = glyph_top;
        if (glyph_bottom < top) top = glyph_bottom;
    }
    if (top == INT_MAX) return;
    const int baseline = centre_y + (bottom + top) / 2;

    /* Two passes, so every outline stays behind every fill. */
    for (int pass = 0; pass < 2; pass++) {
        int pen_x = centre_x - text_width_uncached(text, pixels) / 2;
        if (FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)pixels) != 0) return;
        cursor = (const unsigned char*)text;
        while (*cursor) {
            FT_ULong codepoint = text_codepoint(&cursor);
            if (FT_Load_Char(g_face, codepoint, FT_LOAD_RENDER) != 0)
                continue;
            const FT_GlyphSlot glyph = g_face->glyph;
            if (g_ui_emit && pass == 0) {
                // Vector stroke at drawable scale: rasterizing a wide outline
                // once avoids O(radius^2) disk compositing at 4K/HiDPI sizes.
                FT_Glyph outline = NULL;
                FT_Stroker stroker = NULL;
                if (FT_Load_Char(g_face, codepoint, FT_LOAD_NO_BITMAP) == 0 &&
                    FT_Get_Glyph(g_face->glyph, &outline) == 0 &&
                    FT_Stroker_New(g_library, &stroker) == 0) {
                    FT_Stroker_Set(stroker, g_outline_radius * 64,
                        FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);
                    if (FT_Glyph_StrokeBorder(&outline, stroker, 0, 1) == 0 &&
                        FT_Glyph_To_Bitmap(&outline, FT_RENDER_MODE_NORMAL, NULL, 1) == 0) {
                        FT_BitmapGlyph bitmap = (FT_BitmapGlyph)outline;
                        for (unsigned y = 0; y < bitmap->bitmap.rows; ++y)
                            for (unsigned x = 0; x < bitmap->bitmap.width; ++x)
                                put_pixel(pen_x + bitmap->left + x,
                                    baseline - bitmap->top + y, COLOR_TEXT_OUTLINE,
                                    bitmap->bitmap.buffer[y * bitmap->bitmap.pitch + x]);
                    }
                }
                if (stroker) FT_Stroker_Done(stroker);
                if (outline) FT_Done_Glyph(outline);
            } else {
                draw_glyph(&glyph->bitmap, pen_x + glyph->bitmap_left,
                           baseline - glyph->bitmap_top, pass == 0);
            }
            pen_x += (int)(glyph->advance.x >> 6);
        }
    }
}

/* Runs are keyed by UTF-8 text and pixel size. Font/colours are fixed for the
 * process, and g_lock protects both FreeType and the cache. Metrics-only hits
 * also avoid repeatedly loading glyphs during fitting and alignment. */
enum { TEXT_CACHE_COUNT = 256, TEXT_CACHE_KEY_BYTES = 512,
       TEXT_CACHE_MAX_BYTES = 16 * 1024 * 1024 };
typedef struct text_cache_entry {
    char text[TEXT_CACHE_KEY_BYTES];
    int pixels, outline, advance, baseline_shift;
    uint64_t texture_id;
    int left, top, width, height, rasterized;
    uint32_t hash;
    uint64_t used;
    uint32_t* bitmap;
} text_cache_entry;
static text_cache_entry g_text_cache[TEXT_CACHE_COUNT];
static size_t g_text_cache_bytes;
static uint64_t g_text_cache_clock, g_text_texture_id;

static void release_text_bitmap(text_cache_entry* entry)
{
    if (entry->bitmap) {
        g_text_cache_bytes -= (size_t)entry->width * entry->height * sizeof(uint32_t);
        free(entry->bitmap);
        entry->bitmap = NULL;
    }
    entry->rasterized = 0;
}

static text_cache_entry* get_text(const char* text, int pixels)
{
    if (!text || !text[0] || pixels <= 0) return NULL;
    const size_t length = strlen(text);
    if (length >= TEXT_CACHE_KEY_BYTES) return NULL;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; ++i) hash = (hash ^ (unsigned char)text[i]) * 16777619u;
    text_cache_entry* oldest = &g_text_cache[0];
    for (unsigned i = 0; i < TEXT_CACHE_COUNT; ++i) {
        text_cache_entry* entry = &g_text_cache[i];
        if (entry->pixels == pixels && entry->outline == g_outline_radius && entry->hash == hash && !strcmp(entry->text, text)) {
            entry->used = ++g_text_cache_clock;
            return entry;
        }
        if (entry->used < oldest->used) oldest = entry;
    }
    if (FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)pixels) != 0) return NULL;
    release_text_bitmap(oldest);
    memset(oldest, 0, sizeof(*oldest));
    memcpy(oldest->text, text, length + 1);
    oldest->pixels = pixels;
    oldest->outline = g_outline_radius;
    oldest->hash = hash;
    oldest->used = ++g_text_cache_clock;
    int top = INT_MAX, bottom = 0;
    const unsigned char* cursor = (const unsigned char*)text;
    while (*cursor) {
        if (FT_Load_Char(g_face, text_codepoint(&cursor), FT_LOAD_DEFAULT) != 0) continue;
        const FT_Glyph_Metrics* metrics = &g_face->glyph->metrics;
        const int glyph_top = (int)(metrics->horiBearingY >> 6);
        const int glyph_bottom = glyph_top - (int)(metrics->height >> 6);
        if (glyph_top > bottom) bottom = glyph_top;
        if (glyph_bottom < top) top = glyph_bottom;
        oldest->advance += (int)(g_face->glyph->advance.x >> 6);
    }
    oldest->baseline_shift = top == INT_MAX ? 0 : (top + bottom) / 2;
    return oldest;
}

static int rasterize_text(text_cache_entry* entry)
{
    if (entry->rasterized) return 1;
    if (FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)entry->pixels) != 0) return 0;
    int left = 0, right = 0, top = 0, bottom = 0, pen = 0;
    const unsigned char* cursor = (const unsigned char*)entry->text;
    while (*cursor) {
        if (FT_Load_Char(g_face, text_codepoint(&cursor), FT_LOAD_RENDER) != 0) continue;
        const FT_GlyphSlot glyph = g_face->glyph;
        if (glyph->bitmap.width && glyph->bitmap.rows) {
            const int x = pen + glyph->bitmap_left, y = -glyph->bitmap_top;
            if (x < left) left = x;
            if (y < top) top = y;
            if (x + (int)glyph->bitmap.width > right) right = x + (int)glyph->bitmap.width;
            if (y + (int)glyph->bitmap.rows > bottom) bottom = y + (int)glyph->bitmap.rows;
        }
        pen += (int)(glyph->advance.x >> 6);
    }
    entry->left = left - g_outline_radius;
    entry->top = top - g_outline_radius;
    entry->width = right - left + 2 * g_outline_radius;
    entry->height = bottom - top + 2 * g_outline_radius;
    if (entry->width > 16384 || entry->height > 4096) return 0;
    const size_t bytes = (size_t)entry->width * entry->height * sizeof(uint32_t);
    if (bytes > TEXT_CACHE_MAX_BYTES) return 0;
    while (g_text_cache_bytes + bytes > TEXT_CACHE_MAX_BYTES) {
        text_cache_entry* oldest = NULL;
        for (unsigned i = 0; i < TEXT_CACHE_COUNT; ++i) {
            text_cache_entry* candidate = &g_text_cache[i];
            if (candidate == entry || !candidate->bitmap) continue;
            if (!oldest || candidate->used < oldest->used) oldest = candidate;
        }
        if (!oldest) return 0;
        release_text_bitmap(oldest);
    }
    entry->bitmap = (uint32_t*)calloc(1, bytes);
    if (!entry->bitmap) return 0;
    g_text_cache_bytes += bytes;
    // Use the original two-pass outline/fill rasterizer on a transparent run.
    // This retains outline ordering and glyph bearings, including Japanese text.
    uint32_t* frame = g_pixels;
    const int frame_width = g_width, frame_height = g_height;
    g_pixels = entry->bitmap;
    g_width = entry->width;
    g_height = entry->height;
    draw_text_uncached(entry->text, entry->pixels,
                       entry->advance / 2 - entry->left,
                       -entry->top - entry->baseline_shift);
    g_pixels = frame;
    g_width = frame_width;
    g_height = frame_height;
    entry->texture_id = ++g_text_texture_id;
    entry->rasterized = 1;
    return 1;
}

static int text_width(const char* text, int pixels)
{
    text_cache_entry* entry = get_text(text, pixels);
    return entry ? entry->advance : text_width_uncached(text, pixels);
}

static void draw_text_at(const char* text, int pixels, float centre_x, float centre_y)
{
    if (g_ui_emit) {
        const int native_pixels = (int)ceilf(pixels * g_ui_scale);
        g_outline_radius = (int)ceilf(TEXT_OUTLINE_RADIUS * g_ui_scale);
        text_cache_entry* native = get_text(text, native_pixels);
        if (native && rasterize_text(native)) {
            HostUiDraw draw = {0};
            draw.x = centre_x + (native->left - native->advance / 2) / g_ui_scale;
            draw.y = centre_y + (native->top + native->baseline_shift) / g_ui_scale;
            draw.w = native->width / g_ui_scale;
            draw.h = native->height / g_ui_scale;
            draw.colour = 0xffffffffu;
            draw.texture_id = native->texture_id;
            draw.pixels = native->bitmap;
            draw.width = native->width;
            draw.height = native->height;
            g_ui_emit(g_ui_user, &draw);
        }
        g_outline_radius = TEXT_OUTLINE_RADIUS;
        return;
    }
    text_cache_entry* entry = get_text(text, pixels);
    if (!entry || !rasterize_text(entry)) {
        draw_text_uncached(text, pixels, centre_x, centre_y);
        return;
    }
    const int left = centre_x - entry->advance / 2 + entry->left;
    const int top = centre_y + entry->baseline_shift + entry->top;
    for (int y = 0; y < entry->height; ++y) {
        if (top + y < 0 || top + y >= g_height) continue;
        const uint32_t* row = entry->bitmap + (size_t)y * entry->width;
        for (int x = 0; x < entry->width; ++x) {
            if (left + x < 0 || left + x >= g_width) continue;
            const uint32_t colour = row[x];
            if ((colour >> 24) == 255)
                g_pixels[(size_t)(top + y) * g_width + left + x] = colour;
            else if (colour >> 24)
                put_pixel(left + x, top + y, colour, 255);
        }
    }
}

static void draw_text_fit(const char* text, int preferred_pixels,
                          int maximum_width, float centre_x, float centre_y)
{
    int pixels = preferred_pixels;
    while (pixels > 20 && text_width(text, pixels) > maximum_width)
        pixels -= 2;
    draw_text_at(text, pixels, centre_x, centre_y);
}

static void draw_text_left_fit(const char* text, int preferred_pixels,
                               int maximum_width, float left, float centre_y)
{
    int pixels = preferred_pixels;
    while (pixels > 15 && text_width(text, pixels) > maximum_width)
        pixels -= 2;
    draw_text_at(text, pixels, left + text_width(text, pixels) / 2,
                 centre_y);
}

static void draw_text_right(const char* text, int pixels, float right,
                            float centre_y)
{
    draw_text_at(text, pixels, right - text_width(text, pixels) / 2,
                 centre_y);
}

static void fill_rect(float left, float top, float right, float bottom, uint32_t colour)
{
    if (g_ui_emit) {
        HostUiDraw draw = {0};
        draw.x = left; draw.y = top; draw.w = right - left; draw.h = bottom - top;
        draw.radius = 0; draw.colour = colour;
        g_ui_emit(g_ui_user, &draw);
        return;
    }
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > g_width) right = g_width;
    if (bottom > g_height) bottom = g_height;
    for (int y = top; y < bottom; ++y)
        for (int x = left; x < right; ++x)
            g_pixels[(size_t)y * g_width + x] = colour;
}

static void fill_rounded_rect(float left, float top, float right, float bottom,
                              int radius, uint32_t colour)
{
    if (g_ui_emit) {
        HostUiDraw draw = {0};
        draw.x = left; draw.y = top; draw.w = right - left; draw.h = bottom - top;
        draw.radius = radius; draw.colour = colour;
        g_ui_emit(g_ui_user, &draw);
        return;
    }
    if (radius <= 0) {
        fill_rect(left, top, right, bottom, colour);
        return;
    }
    fill_rect(left + radius, top, right - radius, bottom, colour);
    fill_rect(left, top + radius, right, bottom - radius, colour);
    const int radius_squared = radius * radius;
    for (int y = 0; y < radius; ++y) {
        for (int x = 0; x < radius; ++x) {
            const int dx = radius - x - 1;
            const int dy = radius - y - 1;
            if (dx * dx + dy * dy > radius_squared) continue;
            put_pixel(left + x, top + y, colour, 255);
            put_pixel(right - x - 1, top + y, colour, 255);
            put_pixel(left + x, bottom - y - 1, colour, 255);
            put_pixel(right - x - 1, bottom - y - 1, colour, 255);
        }
    }
}

static uint32_t genre_colour(const char* genre)
{
    static const uint32_t palette[] = {
        RGB_COLOUR(0xF0, 0x6A, 0x9B), RGB_COLOUR(0x6B, 0xC6, 0xE8),
        RGB_COLOUR(0x9B, 0x7B, 0xE8), RGB_COLOUR(0x62, 0xC7, 0xA5),
        RGB_COLOUR(0xF2, 0x9D, 0x50), RGB_COLOUR(0xDF, 0x6B, 0x6B),
        RGB_COLOUR(0x78, 0xA7, 0xF2),
    };
    uint32_t hash = 2166136261u;
    for (const unsigned char* cursor = (const unsigned char*)genre;
         cursor && *cursor; ++cursor) {
        hash ^= *cursor;
        hash *= 16777619u;
    }
    return palette[hash % (sizeof(palette) / sizeof(palette[0]))];
}

static void render_host(void)
{
    g_width = HOST_WIDTH;
    g_height = HOST_HEIGHT;
    fill_rect(0, 0, g_width, g_height, 0xFF24170Fu);
    fill_rect(0, 0, g_width, 150, 0xFF5A2415u);
    fill_rect(0, 145, g_width, 150, 0xFF40C8FFu);

    if (g_mode == 3) {
        draw_text_at("PLAYER LOGIN", 58, HOST_WIDTH / 2, 82);
        const uint32_t selected = 0xFF36B7F3u;
        const uint32_t idle = 0xFF574232u;
        fill_rect(185, 235, 615, 455, g_host_selection == 0 ? selected : idle);
        fill_rect(665, 235, 1095, 455, g_host_selection == 1 ? selected : idle);
        draw_text_at("ANONYMOUS", 42, 400, 330);
        draw_text_at("NO CARD - OFFLINE", 25, 400, 392);
        draw_text_at("BANAPASSPORT", 42, 880, 330);
        draw_text_at("USE YOUR SAVED PROFILE", 25, 880, 392);
        draw_text_at("RIM: CHOOSE     CENTRE: CONFIRM", 29,
                     HOST_WIDTH / 2, 580);
        return;
    }

    if (g_mode == 4) {
        draw_text_at("BANAPASSPORT LOGIN", 54, HOST_WIDTH / 2, 82);
        draw_text_at("ENTER THIS CODE ON THE PAIRING PAGE", 31,
                     HOST_WIDTH / 2, 245);
        if (g_code[0]) {
            char code[16];
            if (strlen(g_code) == 6)
                snprintf(code, sizeof(code), "%.3s-%s", g_code, g_code + 3);
            else
                snprintf(code, sizeof(code), "%s", g_code);
            draw_text_at(code, 88, HOST_WIDTH / 2, 365);
            char countdown[48];
            int remaining = (int)(g_deadline - monotonic_seconds());
            if (remaining < 0) remaining = 0;
            snprintf(countdown, sizeof(countdown), "EXPIRES IN %d", remaining);
            draw_text_at(countdown, 29, HOST_WIDTH / 2, 465);
        } else {
            draw_text_at("WAITING FOR SERVER...", 46, HOST_WIDTH / 2, 365);
        }
        draw_text_at("THE GAME IS AUTHENTICATING THROUGH ITS NATIVE CARD PATH",
                     24, HOST_WIDTH / 2, 580);
        return;
    }

    if (g_mode == 6) {
        draw_text_at("STARTING SESSION", 58, HOST_WIDTH / 2, 82);
        if (g_player_name[0])
            draw_text_at(g_player_name, 40, HOST_WIDTH / 2, 315);
        draw_text_at("PLAYER DATA IS READY", 34, HOST_WIDTH / 2, 400);
        draw_text_at("FINISHING THE NATIVE GAME HANDOFF...", 27,
                     HOST_WIDTH / 2, 535);
        return;
    }

    /* Persistent song details and an animated song/difficulty carousel. Only
     * the 180 ms input transitions redraw; settled screens retain their frame. */
    const float ease = song_ease();
    int expanded = 0;
    for (unsigned r = 0; r < g_song_row_count; ++r)
        expanded |= g_song_rows[r].kind == TAIKO_OVERLAY_ROW_DIFFICULTY;
    fill_rect(0, 0, g_width, g_height, RGB_COLOUR(0x10, 0x18, 0x25));
    fill_rect(0, 0, 570, 660, RGB_COLOUR(0x19, 0x28, 0x3A));
    fill_rect(570, 0, g_width, 660, RGB_COLOUR(0x11, 0x1B, 0x29));
    fill_rect(0, 0, 570, 7, RGB_COLOUR(0xF0, 0x6A, 0x9B));
    fill_rect(570, 0, g_width, 7, RGB_COLOUR(0x6B, 0xC6, 0xE8));
    for (int y = 0; y < 660; y += 80)
        fill_rect(570, y, g_width, y + 1, RGB_COLOUR(0x1D, 0x2A, 0x3A));

    draw_text_left_fit(g_song_browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES
                           ? "CATEGORY SELECT" : "SONG SELECT",
                       39, 440, 34, 49);
    draw_text_left_fit("TAIKO GREEN / HOST LIBRARY", 17, 480, 35, 82);
    char category[112];
    if (g_song_browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES)
        snprintf(category, sizeof(category), "CHOOSE A GENRE FOLDER");
    else if (!g_song_category_total)
        snprintf(category, sizeof(category), "[ SEARCH RESULTS ]");
    else
        snprintf(category, sizeof(category), "[ %s ]  FOLDER %u/%u",
                 g_song_category[0] ? g_song_category : "SONGS",
                 g_song_category_index + 1, g_song_category_total);
    draw_text_left_fit(category, 17, 370, 35, 108);
    draw_text_right(g_player_name, 13, 535, 108);

    fill_rounded_rect(625, 24, 1248, 91, 12,
                      g_song_search_active
                          ? RGB_COLOUR(0x35, 0x5D, 0x78)
                          : RGB_COLOUR(0x25, 0x37, 0x4A));
    fill_rect(625, 87, 1248, 91,
              g_song_search_active
                  ? RGB_COLOUR(0xF0, 0x6A, 0x9B)
                  : RGB_COLOUR(0x6B, 0xC6, 0xE8));
    char search[192];
    if (g_song_browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES)
        snprintf(search, sizeof(search),
                 "TAB / CTRL+F  SEARCH ALL SONGS");
    else if (g_song_query[0])
        snprintf(search, sizeof(search), "SEARCH: %s%s", g_song_query,
                 g_song_search_active ? "_" : "");
    else
        snprintf(search, sizeof(search), "%s",
                 g_song_search_active ? "TYPE TO FILTER..._"
                                      : "TAB / CTRL+F TO SEARCH");
    draw_text_left_fit(search, 23, 390, 648, 56);
    char result_count[80];
    if (g_song_browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES)
        snprintf(result_count, sizeof(result_count), "%u CATEGORIES",
                 g_song_match_total);
    else
        snprintf(result_count, sizeof(result_count), "%u SONGS",
                 g_song_match_total);
    draw_text_right(result_count, 18, 1224, 58);

    if (!g_song_catalog_total) {
        draw_text_at("CATALOG UNAVAILABLE", 42, 900, 320);
        draw_text_at("CHECK PS3_VFS_ROOT AND MUSICINFO.XML", 23, 900, 375);
        return;
    }

    if (!g_song_match_total &&
        g_song_browser_level == TAIKO_OVERLAY_BROWSER_SONGS) {
        draw_text_at("NO MATCHES", 43, 910, 313);
        draw_text_at("BACKSPACE TO EDIT OR ESC TO CLEAR", 22, 910, 369);
    } else {
        fill_rounded_rect(28, 125, 537, 379, 14,
                          RGB_COLOUR(0x23, 0x37, 0x4B));
        fill_rect(28, 125, 36, 379,
                  genre_colour(g_song_browser_level ==
                                       TAIKO_OVERLAY_BROWSER_CATEGORIES
                                   ? g_song_title : g_song_genre));
        if (g_song_browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES) {
            draw_text_left_fit(g_song_title, 47, 445, 57, 205);
            char folder_count[80];
            snprintf(folder_count, sizeof(folder_count), "%u SONGS",
                     g_song_unique_id);
            draw_text_left_fit(folder_count, 26, 450, 57, 292);
            draw_text_left_fit("ENTER OR RIGHT CENTRE  OPEN", 19, 490,
                               34, 455);
            draw_text_left_fit("RIM / WHEEL  CHOOSE CATEGORY", 17, 490,
                               34, 495);
        } else if (g_song_selection_is_exit) {
            draw_text_left_fit(g_song_genre, 21, 450, 57, 160);
            draw_text_left_fit("BACK TO CATEGORIES", 39, 445, 57, 225);
            draw_text_left_fit("RETURN TO THE FOLDER LIST", 21, 450,
                               57, 303);
            draw_text_left_fit("ENTER OR RIGHT CENTRE  EXIT", 19, 490,
                               34, 455);
        } else {
            draw_text_left_fit(g_song_genre[0] ? g_song_genre : "OTHER", 21,
                               450, 57, 160);
            draw_text_left_fit(g_song_title, 41, 445, 57, 225);

            char identity[160];
            snprintf(identity, sizeof(identity), "ID  %s     UNIQUE  %u",
                     g_song_id, g_song_unique_id);
            draw_text_left_fit(identity, 19, 450, 57, 303);
            char position[80];
            snprintf(position, sizeof(position), "SONG %u OF %u",
                     g_song_index + 1, g_song_match_total);
            draw_text_left_fit(position, 19, 450, 57, 345);

            draw_text_left_fit(expanded ? "RIMS / UP / DOWN  CHOOSE CHART"
                                        : "RIMS / UP / DOWN  CHOOSE SONG", 18, 500, 34, 415);
            draw_text_left_fit(expanded ? "RIGHT CENTRE / ENTER  READY"
                                        : "RIGHT CENTRE / ENTER  OPEN SONG", 18, 500, 34, 451);
            draw_text_left_fit(expanded ? "LEFT CENTRE / ESC  CLOSE SONG"
                                        : "LEFT CENTRE / ESC  CATEGORIES", 17, 500, 34, 487);

        }

        if (g_browser_players_enabled) {
            const int songs = g_song_browser_level == TAIKO_OVERLAY_BROWSER_SONGS &&
                              !g_song_selection_is_exit;
            for (unsigned slot = 0; slot < 2; ++slot) {
                const int top = 520 + (int)slot * 64;
                const int joined = (g_browser_joined & (1u << slot)) != 0;
                const uint32_t colour = slot ? RGB_COLOUR(0x32, 0x80, 0xAC)
                                             : RGB_COLOUR(0xB6, 0x46, 0x55);
                fill_rounded_rect(28, top, 537, top + 57, 9,
                    joined ? colour : RGB_COLOUR(0x29, 0x39, 0x49));
                char line[112];
                snprintf(line, sizeof line, "P%u  %s", slot + 1,
                    !joined ? "HIT DRUM TO JOIN" : "JOINED");
                draw_text_left_fit(line, 22, 345, 43, top + 27);
                if (joined && songs && expanded)
                    draw_text_right((g_browser_ready & (1u << slot)) ? "READY" : "CHOOSE", 17, 523, top + 28);
            }
        }

        const int first_y = 111;
        const int row_step = 59;
        for (unsigned row = 0; row < g_song_row_count; ++row) {
            const song_row_storage* item = &g_song_rows[row];
            const int target_y = first_y + (int)row * row_step;
            const float top = item->from_y + (target_y - item->from_y) * ease;
            const int target_x = row_target_x(item->kind, item->selected);
            const float left = item->from_x + (target_x - item->from_x) * ease;
            if (top < 100 || top > 607) continue;
            if (item->kind == TAIKO_OVERLAY_ROW_DIFFICULTY) {
                fill_rounded_rect(left, top, 1252, top + 53, 10,
                    item->selected ? RGB_COLOUR(0x3C, 0x52, 0x69) : RGB_COLOUR(0x22, 0x30, 0x42));
                draw_text_left_fit(item->title, 22, 165, left + 18, top + 27);
                char rating[24];
                if (item->stars) snprintf(rating, sizeof rating, "★ %u", item->stars);
                else snprintf(rating, sizeof rating, "★ --");
                draw_text_right(rating, 20, 1032, top + 27);
                for (unsigned p = 0; p < 2; ++p) {
                    if (!(item->cursors & (1u << p))) continue;
                    const int x = 1052 + p * 94;
                    fill_rounded_rect(x, top + 8, x + 88, top + 45, 8,
                        p ? RGB_COLOUR(0x32, 0xA8, 0xDA) : RGB_COLOUR(0xE5, 0x59, 0x73));
                    char badge[20];
                    snprintf(badge, sizeof badge, "P%u%s", p + 1,
                             item->ready & (1u << p) ? " OK" : " <");
                    draw_text_at(badge, 17, x + 44, top + 27);
                }
                continue;
            }
            const uint32_t colour = item->kind == TAIKO_OVERLAY_ROW_EXIT
                ? (item->selected ? RGB_COLOUR(0xD8, 0x58, 0x70)
                                  : RGB_COLOUR(0x4B, 0x2A, 0x38))
                : item->selected ? genre_colour(item->genre)
                                 : RGB_COLOUR(0x29, 0x3A, 0x4D);
            fill_rounded_rect(left, top, 1252, top + 53, 10, colour);
            if (!item->selected)
                fill_rect(left, top, left + 5, top + 53,
                          genre_colour(item->genre));
            if (item->kind == TAIKO_OVERLAY_ROW_CATEGORY) {
                draw_text_left_fit(item->title, item->selected ? 28 : 25,
                                   item->selected ? 470 : 450,
                                   left + 18, top + 27);
            } else {
                draw_text_left_fit(item->title, item->selected ? 22 : 19,
                                   item->selected ? 470 : 450,
                                   left + 18, top + 19);
                draw_text_left_fit(
                    item->kind == TAIKO_OVERLAY_ROW_EXIT
                        ? "RETURN TO THE CATEGORY LIST" : item->genre,
                    15, 350, left + 19, top + 40);
            }
            char number[24];
            if (item->kind == TAIKO_OVERLAY_ROW_CATEGORY)
                snprintf(number, sizeof(number), "%u SONGS",
                         item->catalog_index);
            else if (item->kind == TAIKO_OVERLAY_ROW_EXIT)
                snprintf(number, sizeof(number), "EXIT");
            else
                snprintf(number, sizeof(number), "%03u",
                         item->catalog_index + 1);
            draw_text_right(number, 17, 1231, top + 27);
        }
    }

    fill_rect(0, 660, g_width, g_height, RGB_COLOUR(0x0B, 0x11, 0x1B));
    draw_text_left_fit(expanded ? "RIM / WHEEL  CHOOSE CHART" : "RIM / WHEEL  BROWSE",
                       18, 290, 30, 690);
    draw_text_at(g_browser_save_status[0] ? g_browser_save_status :
                 g_song_browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES
                     ? "ENTER  OPEN FOLDER"
                     : expanded ? "P1 RED / P2 BLUE   CHOOSE YOUR CHART"
                                : "UP/DOWN  BROWSE     PAGEUP/DOWN  SKIP",
                 16, 655, 690);
    draw_text_right(g_song_browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES
                        ? "9 ORIGINAL CATEGORIES"
                        : expanded ? "ESC  CLOSE SONG" : "ESC  BACK / CLEAR FILTER",
                    17, 1245, 690);
}

static void finish_handoff_if_due(void)
{
    if (g_mode != 5) { g_handoff = 0; g_handoff_snapshot = 0; }
    if (!g_handoff || !g_handoff_snapshot ||
        monotonic_milliseconds() - g_handoff_start < HANDOFF_MS) return;
    if (g_handoff < 0) { g_mode = 0; g_visible = 0; }
    g_handoff = 0;
    g_handoff_snapshot = 0;
    g_drawn_remaining = -1;
    ++g_version;
}

static void render_handoff(void)
{
    static const int xs[3] = {0, 570, 0}, ys[3] = {0, 0, 660};
    static const int widths[3] = {570, 710, 1280}, heights[3] = {660, 660, 60};
    if (!g_handoff_snapshot) {
        HostUiEmit emit = g_ui_emit;
        g_ui_emit = NULL;
        if (g_handoff > 0) g_song_animation_start = monotonic_milliseconds() - 180.0;
        render_host();
        size_t offset = 0;
        for (unsigned panel = 0; panel < 3; ++panel) {
            for (int y = 0; y < heights[panel]; ++y)
                memcpy(g_handoff_pixels + offset + y * widths[panel],
                       g_pixels + (ys[panel] + y) * HOST_WIDTH + xs[panel],
                       widths[panel] * sizeof(uint32_t));
            offset += widths[panel] * heights[panel];
            g_handoff_ids[panel] = ++g_text_texture_id;
        }
        g_ui_emit = emit;
        g_handoff_snapshot = 1;
        /* Begin on the first rendered frame, not before synchronous loading. */
        g_handoff_start = monotonic_milliseconds();
    }
    double t = (monotonic_milliseconds() - g_handoff_start) / HANDOFF_MS;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const double eased = t * t * (3.0 - 2.0 * t);
    const float hidden = (float)(g_handoff < 0 ? eased : 1.0 - eased);
    const unsigned alpha = (unsigned)(255.0f * (1.0f - hidden) + 0.5f);
    g_width = HOST_WIDTH; g_height = HOST_HEIGHT;
    if (!g_ui_emit || g_handoff > 0)
        fill_rect(0, 0, HOST_WIDTH, HOST_HEIGHT, g_handoff > 0 ? 0xff000000u : 0);
    size_t offset = 0;
    for (unsigned panel = 0; panel < 3; ++panel) {
        const int slide = (int)(96 * hidden + 0.5f);
        const int x = xs[panel] + (panel == 0 ? -slide : panel == 1 ? slide : 0);
        const int y = ys[panel] + (panel == 2 ? (int)(48 * hidden + 0.5f) : 0);
        const uint32_t* pixels = g_handoff_pixels + offset;
        if (g_ui_emit) {
            HostUiDraw draw = {0};
            draw.x = x; draw.y = y; draw.w = widths[panel]; draw.h = heights[panel];
            draw.colour = (alpha << 24) | 0xffffffu;
            draw.texture_id = g_handoff_ids[panel]; draw.pixels = pixels;
            draw.width = widths[panel]; draw.height = heights[panel];
            g_ui_emit(g_ui_user, &draw);
        } else {
            for (int py = 0; py < heights[panel] && y + py < HOST_HEIGHT; ++py) {
                for (int px = 0; px < widths[panel]; ++px) {
                    if (x + px < 0 || x + px >= HOST_WIDTH) continue;
                    uint32_t colour = pixels[py * widths[panel] + px];
                    if (g_handoff > 0) {
                        uint32_t faded = 0xff000000u;
                        for (unsigned shift = 0; shift < 24; shift += 8)
                            faded |= (((colour >> shift) & 255) * alpha / 255) << shift;
                        colour = faded;
                    } else colour = (colour & 0xffffffu) | (alpha << 24);
                    g_pixels[(y + py) * HOST_WIDTH + x + px] = colour;
                }
            }
        }
        offset += widths[panel] * heights[panel];
    }
}

static void render(int remaining)
{
    char code[16];
    char countdown[4];

    if (g_mode >= 3) {
        if (g_handoff) render_handoff();
        else render_host();
        g_drawn_remaining = remaining;
        ++g_version;
        return;
    }

    draw_pill();

    if (g_mode == 2) {
        draw_text_at(g_status, 24, PILL_WIDTH / 2, PILL_HEIGHT / 2);
        g_drawn_remaining = remaining;
        ++g_version;
        return;
    }

    /* 661722 reads as 661-722 on the cabinet. */
    if (strlen(g_code) == 6)
        snprintf(code, sizeof(code), "%.3s-%s", g_code, g_code + 3);
    else
        snprintf(code, sizeof(code), "%s", g_code);
    draw_text_at(code, CODE_PIXELS, CODE_CENTER_X, PILL_HEIGHT / 2);

    snprintf(countdown, sizeof(countdown), "%d", remaining > 99 ? 99 : remaining);
    draw_text_at(countdown, COUNTDOWN_PIXELS, PILL_DISC_WIDTH / 2,
                 PILL_HEIGHT / 2);

    g_drawn_remaining = remaining;
    ++g_version;
}

/* --------------------------------------------------------------------------
 * Public entry points
 * -----------------------------------------------------------------------*/

void taiko_overlay_set_pairing(const char* code, int expires_in)
{
    pthread_mutex_lock(&g_lock);
    /* The reader polls before the user chooses BAID. Its legacy pairing pill
     * must not replace an opaque host-owned screen. Mode 4 is the exception:
     * it is the host BAID screen and consumes the refreshed code itself. */
    if (g_mode >= 3 && g_mode != 4) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    snprintf(g_code, sizeof(g_code), "%s", code ? code : "");
    g_deadline = monotonic_seconds() + (expires_in > 0 ? expires_in : 0);
    g_visible = g_code[0] != '\0';
    if (g_mode != 4) g_mode = 1;
    g_drawn_remaining = -1;
    pthread_mutex_unlock(&g_lock);
    wake_renderer();
}

void taiko_overlay_set_status(const char* text, int expires_in)
{
    pthread_mutex_lock(&g_lock);
    if (g_mode >= 3) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    snprintf(g_status, sizeof(g_status), "%s", text ? text : "");
    g_deadline = monotonic_seconds() + (expires_in > 0 ? expires_in : 0);
    g_visible = g_status[0] != '\0';
    g_mode = 2;
    g_drawn_remaining = -1;
    pthread_mutex_unlock(&g_lock);
    wake_renderer();
}

void taiko_overlay_set_browser_save_status(const char* text)
{
    pthread_mutex_lock(&g_lock);
    if (strcmp(g_browser_save_status, text ? text : "") != 0) {
        snprintf(g_browser_save_status, sizeof(g_browser_save_status), "%s",
                 text ? text : "");
        g_drawn_remaining = -1;
    }
    pthread_mutex_unlock(&g_lock);
    wake_renderer();
}

void taiko_overlay_show_entry_menu(int selection)
{
    pthread_mutex_lock(&g_lock);
    g_host_selection = selection != 0;
    g_mode = 3;
    g_visible = 1;
    g_deadline = 0;
    g_drawn_remaining = -1;
    ++g_version;
    pthread_mutex_unlock(&g_lock);
    wake_renderer();
}

void taiko_overlay_show_baid_wait(void)
{
    pthread_mutex_lock(&g_lock);
    g_mode = 4;
    g_visible = 1;
    g_drawn_remaining = -1;
    ++g_version;
    pthread_mutex_unlock(&g_lock);
    wake_renderer();
}

void taiko_overlay_show_entry_progress(const char* player_name)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_player_name, sizeof(g_player_name), "%s",
             player_name && player_name[0] ? player_name : "P1");
    g_mode = 6;
    g_visible = 1;
    g_deadline = 0;
    g_drawn_remaining = -1;
    ++g_version;
    pthread_mutex_unlock(&g_lock);
    wake_renderer();
}

void taiko_overlay_show_song_select(const char* player_name)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_player_name, sizeof(g_player_name), "%s",
             player_name && player_name[0] ? player_name : "P1");
    g_song_match_total = 0;
    g_song_catalog_total = 0;
    snprintf(g_song_category, sizeof(g_song_category), "ALL SONGS");
    g_song_category_index = 0;
    g_song_category_total = 1;
    g_song_row_count = 0;
    g_song_query[0] = '\0';
    g_song_search_active = 0;
    g_song_browser_level = TAIKO_OVERLAY_BROWSER_CATEGORIES;
    g_song_selection_is_exit = 0;
    g_song_animating = 0;
    g_mode = 5;
    g_visible = 1;
    g_deadline = 0;
    g_drawn_remaining = -1;
    ++g_version;
    pthread_mutex_unlock(&g_lock);
    wake_renderer();
}

void taiko_overlay_set_browser_players(int enabled, uint8_t joined, uint8_t ready,
                                      const uint8_t difficulties[2])
{
    pthread_mutex_lock(&g_lock);
    g_browser_players_enabled = enabled;
    g_browser_joined = joined;
    g_browser_ready = ready;
    for (unsigned i = 0; i < 2; ++i)
        g_browser_difficulties[i] = difficulties ? difficulties[i] : 0;
    pthread_mutex_unlock(&g_lock);
}

void taiko_overlay_show_song_browser(const char* player_name,
                                     const char* music_id,
                                     const char* title,
                                     const char* genre,
                                     uint32_t unique_id,
                                     unsigned index,
                                     unsigned match_total,
                                     unsigned catalog_total,
                                     const char* category,
                                     unsigned category_index,
                                     unsigned category_total,
                                     const char* difficulty,
                                     uint8_t difficulty_mask,
                                     const char* query,
                                     int search_active,
                                     int browser_level,
                                     int selection_is_exit,
                                     const taiko_overlay_song_row* rows,
                                     unsigned row_count)
{
    pthread_mutex_lock(&g_lock);
    /* A fresh selection or a failed launch takes ownership immediately. */
    g_handoff = 0;
    g_handoff_snapshot = 0;
    snprintf(g_player_name, sizeof(g_player_name), "%s",
             player_name && player_name[0] ? player_name : "P1");
    snprintf(g_song_id, sizeof(g_song_id), "%s", music_id ? music_id : "");
    snprintf(g_song_title, sizeof(g_song_title), "%s", title ? title : "");
    snprintf(g_song_genre, sizeof(g_song_genre), "%s", genre ? genre : "");
    snprintf(g_song_difficulty, sizeof(g_song_difficulty), "%s",
             difficulty ? difficulty : "?");
    g_song_unique_id = unique_id;
    g_song_index = index;
    g_song_match_total = match_total;
    g_song_catalog_total = catalog_total;
    snprintf(g_song_category, sizeof(g_song_category), "%s",
             category ? category : "ALL SONGS");
    g_song_category_index = category_index;
    g_song_category_total = category_total;
    g_song_difficulty_mask = difficulty_mask;
    snprintf(g_song_query, sizeof(g_song_query), "%s", query ? query : "");
    g_song_search_active = search_active != 0;
    g_song_browser_level = browser_level;
    g_song_selection_is_exit = selection_is_exit != 0;
    song_row_storage previous[TAIKO_OVERLAY_SONG_ROW_COUNT];
    memcpy(previous, g_song_rows, sizeof previous);
    const unsigned previous_count = g_song_row_count;
    const float old_ease = song_ease();
    int changed = previous_count != row_count;
    g_song_row_count = row_count < TAIKO_OVERLAY_SONG_ROW_COUNT
        ? row_count : TAIKO_OVERLAY_SONG_ROW_COUNT;
    for (unsigned row = 0; row < g_song_row_count; ++row) {
        snprintf(g_song_rows[row].title, sizeof(g_song_rows[row].title), "%s",
                 rows && rows[row].title ? rows[row].title : "");
        snprintf(g_song_rows[row].genre, sizeof(g_song_rows[row].genre), "%s",
                 rows && rows[row].genre ? rows[row].genre : "");
        g_song_rows[row].catalog_index = rows ? rows[row].catalog_index : 0;
        g_song_rows[row].selected = rows && rows[row].selected;
        g_song_rows[row].kind = rows ? rows[row].kind
                                    : TAIKO_OVERLAY_ROW_SONG;
        song_row_storage* item = &g_song_rows[row];
        item->difficulty = rows ? rows[row].difficulty : 0;
        item->stars = rows ? rows[row].stars : 0;
        item->cursors = rows ? rows[row].cursors : 0;
        item->ready = rows ? rows[row].ready : 0;
        item->from_y = 111 + row * 59;
        item->from_x = row_target_x(item->kind, item->selected) + 36;
        int found = -1;
        for (unsigned old = 0; old < previous_count; ++old) {
            const song_row_storage* prior = &previous[old];
            if (prior->kind == item->kind && prior->catalog_index == item->catalog_index &&
                prior->difficulty == item->difficulty && !strcmp(prior->title, item->title)) {
                found = (int)old;
                item->from_y = prior->from_y + (111 + old * 59 - prior->from_y) * old_ease;
                item->from_x = prior->from_x + (row_target_x(prior->kind, prior->selected) - prior->from_x) * old_ease;
                changed |= old != row || prior->selected != item->selected ||
                           prior->cursors != item->cursors || prior->ready != item->ready;
                break;
            }
        }
        changed |= found < 0;
    }
    if (changed) {
        g_song_animation_start = monotonic_milliseconds();
        g_song_animating = 1;
        g_gpu_animation_pending = 1;
    } else {
        /* Repeated publications (including held input) must not restart easing. */
        for (unsigned row = 0; row < g_song_row_count; ++row) {
            g_song_rows[row].from_y = previous[row].from_y;
            g_song_rows[row].from_x = previous[row].from_x;
        }
    }
    g_mode = 5;
    g_visible = 1;
    g_deadline = 0;
    g_drawn_remaining = -1;
    ++g_version;
    pthread_mutex_unlock(&g_lock);
    wake_renderer();
}

void taiko_overlay_hide_host_screen(void)
{
    pthread_mutex_lock(&g_lock);
    g_handoff = 0;
    g_handoff_snapshot = 0;
    if (g_mode >= 3) {
        g_mode = 0;
        g_visible = 0;
        g_code[0] = '\0';
        ++g_version;
    }
    pthread_mutex_unlock(&g_lock);
    wake_renderer();
}

void taiko_overlay_animate_browser(int leaving)
{
    pthread_mutex_lock(&g_lock);
    if (g_visible && g_mode == 5) {
        g_handoff = leaving ? -1 : 1;
        g_handoff_snapshot = 0;
        g_drawn_remaining = -1;
        ++g_version;
    }
    pthread_mutex_unlock(&g_lock);
    wake_renderer();
}

void taiko_overlay_clear(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_mode >= 3) {
        if (g_code[0]) {
            g_code[0] = '\0';
            g_drawn_remaining = -1;
            ++g_version;
        }
        pthread_mutex_unlock(&g_lock);
        wake_renderer();
        return;
    }
    if (g_visible) ++g_version;
    g_visible = 0;
    g_mode = 0;
    g_code[0] = '\0';
    g_status[0] = '\0';
    pthread_mutex_unlock(&g_lock);
    wake_renderer();
}

int taiko_host_frame_copy(HostFrameInfo* info, void* destination,
                          size_t destination_bytes)
{
    if (!info) return 0;
    pthread_mutex_lock(&g_lock);
    finish_handoff_if_due();

    int remaining = (int)(g_deadline - monotonic_seconds());
    if (remaining < 0) remaining = 0;
    if (g_mode == 4 && g_code[0] && remaining == 0) {
        g_code[0] = '\0';
        g_drawn_remaining = -1;
        ++g_version;
    } else if (g_visible && g_mode < 3 && remaining == 0) {
        g_visible = 0;
        ++g_version;
    }
    if (!g_visible || !font_ready()) {
        info->mode = HOST_FRAME_NONE;
        info->width = 0;
        info->height = 0;
        info->pitch = 0;
        info->version = g_version;
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    const double now = monotonic_milliseconds();
    if (remaining != g_drawn_remaining ||
        (g_mode == 5 && (g_song_animating || g_handoff) && now - g_song_last_render >= 16.0)) {
        render(remaining);
        g_song_last_render = now;
        if (song_ease() >= 1.0f) g_song_animating = 0;
    }

    info->mode = g_mode >= 3 && g_handoff >= 0 ? HOST_FRAME_FULLSCREEN : HOST_FRAME_OVERLAY;
    info->width = (uint32_t)g_width;
    info->height = (uint32_t)g_height;
    info->pitch = (uint32_t)g_width * sizeof(uint32_t);
    info->version = g_version;
    if (destination) {
        const size_t required = (size_t)info->pitch * info->height;
        if (destination_bytes < required) {
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        memcpy(destination, g_pixels, required);
    }
    pthread_mutex_unlock(&g_lock);
    return 1;
}

static int visit_host_ui(float scale, HostUiEmit emit, void* user, HostUiInfo* info)
{
    if (!info) return 0;
    pthread_mutex_lock(&g_lock);
    finish_handoff_if_due();
    if (!g_visible || g_mode < 3 || !font_ready()) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (g_mode == 4 && g_code[0] && monotonic_seconds() >= g_deadline) {
        g_code[0] = '\0'; ++g_version; g_drawn_remaining = -1;
    }
    info->version = g_version;
    info->animated = g_mode == 5 && (g_gpu_animation_pending || g_handoff);
    info->overlay = g_handoff < 0;
    if (g_mode == 4 && g_code[0]) info->animated = 1;
    if (emit) {
        g_ui_scale = isfinite(scale) && scale > 0 ? scale : 1.0f;
        g_ui_emit = emit; g_ui_user = user;
        if (g_handoff) render_handoff();
        else render_host();
        if (song_ease() >= 1.0f) g_gpu_animation_pending = 0;
        g_ui_emit = NULL; g_ui_user = NULL;
        g_ui_scale = 1.0f;
    }
    pthread_mutex_unlock(&g_lock);
    return 1;
}
