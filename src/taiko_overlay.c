/* See taiko_overlay.h.
 *
 * The styling follows TaikoZucchini's core/title_render.c (MIT, same author):
 * white fill, and the outline built by disk-dilating the glyph's own coverage
 * mask rather than by stroking the outline, which is what gives the title's
 * text its rounded, even border. A bounded text-run cache retains metrics
 * and transparent outlined bitmaps while browser rows animate.
 */
#include "taiko_overlay.h"
#include "taiko_title_render.h"
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
#include "taiko_menu_art.h"

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
static _Thread_local int g_outline_radius = TEXT_OUTLINE_RADIUS;
static int g_menu_text_outline = TEXT_OUTLINE_RADIUS;
static unsigned g_text_opacity=255;
static HostUiEmit g_ui_emit;
static void* g_ui_user;
static float g_ui_scale = 1.0f;
static struct { uint32_t address, width, height; } g_portraits[2];
static int visit_host_ui(float scale, HostUiEmit emit, void* user, HostUiInfo* info);
extern HostUiVisit g_rsx_host_ui_visit __attribute__((weak));

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_frame_pixels[OVERLAY_MAX_WIDTH * OVERLAY_MAX_HEIGHT];
static _Thread_local uint32_t* g_pixels = g_frame_pixels;
static uint32_t g_version;
static int      g_visible;
static int      g_mode;             /* 1 pairing, 2 status, 3--5 host screens */
static _Thread_local int      g_width = PILL_WIDTH;
static _Thread_local int      g_height = PILL_HEIGHT;
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
static char g_browser_account_names[2][128];
static int g_browser_login_phase;
static char g_browser_login_status[128];
static uint8_t g_browser_authenticated;
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
    uint8_t course_stars[5];
    unsigned browser_position, browser_total, carousel_group;
    float from_y, from_x;
    float from_card_x, from_card_w;
    float from_group_left, from_group_right;
} song_row_storage;
static song_row_storage g_song_rows[TAIKO_OVERLAY_SONG_ROW_COUNT];
static unsigned g_song_row_count;
static int g_song_shared_carousel;
static double g_song_animation_start, g_song_last_render;
static int g_song_animating;
static int g_gpu_animation_pending;
/* Own the outgoing categories across repeated song publications. */
static song_row_storage g_folder_categories[TAIKO_OVERLAY_SONG_ROW_COUNT];
static unsigned g_folder_category_count;
static int g_folder_category_selected;
static double g_folder_open_start;
static int g_folder_open;
static float g_folder_scroll_from, g_folder_scroll_target;
static double g_folder_scroll_start;
static int g_folder_closing;
static double g_folder_close_start;
static song_row_storage g_folder_close_rows[TAIKO_OVERLAY_SONG_ROW_COUNT];
static unsigned g_folder_close_count;
static char g_folder_close_category[64];
static float g_folder_close_left, g_folder_close_right;
static uint8_t g_folder_close_difficulties;
static unsigned g_folder_close_group;
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

static _Thread_local FT_Library g_library;
static _Thread_local FT_Face    g_face;
static _Thread_local int        g_font_state;    /* 0 untried, 1 ready, -1 unavailable */

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

static double text_profile_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec*1000.0+ts.tv_nsec/1000000.0;
}

static double monotonic_milliseconds(void)
{
#ifdef TAIKO_BROWSER_PREVIEW
    extern double taiko_preview_clock_ms;
    if (taiko_preview_clock_ms >= 0) return taiko_preview_clock_ms;
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static int green_categories(void);

static float song_ease(void)
{
    double elapsed=monotonic_milliseconds()-g_song_animation_start;
    if(elapsed<0) elapsed=0;
    if(green_categories()) {
        /* Measured from the 60 Hz reference: eight sliding frames with
         * quadratic ease-out, seven closed frames, fifteen opening frames. */
        if(elapsed<8000.0/60) {
            double t=elapsed/(8000.0/60);
            return (float)(0.45*(1-(1-t)*(1-t)));
        }
        if(elapsed<250) return 0.45f;
        if(elapsed>=500) return 1.0f;
        return (float)(0.45+0.55*(elapsed-250)/250);
    }
    double t=elapsed/180.0;
    if(t>=1) return 1.0f;
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
static _Thread_local int g_cached_outline_pass;
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
            if(g_cached_outline_pass && coverage==255 && row>0 && column>0 &&
               row+1<bitmap->rows && column+1<bitmap->width &&
               bitmap->buffer[(row-1)*bitmap->pitch+column]==255 &&
               bitmap->buffer[(row+1)*bitmap->pitch+column]==255 &&
               bitmap->buffer[row*bitmap->pitch+column-1]==255 &&
               bitmap->buffer[row*bitmap->pitch+column+1]==255) {
                if(px>=0 && py>=0 && px<g_width && py<g_height)
                    g_pixels[(size_t)py*g_width+px]=COLOR_TEXT_OUTLINE;
                continue;
            }
            // All outline samples have one colour. Accumulate alpha directly
            // instead of repeating three straight-alpha colour divisions per
            // overlapping sample. Once opaque, further samples do nothing.
            for (int dy = -g_outline_radius; dy <= g_outline_radius; ++dy) {
                int yy=py+dy;
                if(yy<0 || yy>=g_height)continue;
                int span=g_outline_radius;
                while(span*span+dy*dy>g_outline_radius*g_outline_radius)--span;
                int lo=px-span,hi=px+span;
                if(lo<0)lo=0;
                if(hi>=g_width)hi=g_width-1;
                for(int xx=lo;xx<=hi;++xx) {
                    uint32_t *dst=&g_pixels[(size_t)yy*g_width+xx];
                    if(!g_cached_outline_pass) {
                        put_pixel(xx,yy,COLOR_TEXT_OUTLINE,coverage);
                        continue;
                    }
                    unsigned alpha=*dst>>24;
                    if(alpha==255)continue;
                    alpha=coverage+(alpha*(255-coverage)+127)/255;
                    *dst=(alpha<<24)|(COLOR_TEXT_OUTLINE&0xffffffu);
                }
            }
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
            // Grow the coverage mask, including tight counters. Offset vector
            // contours can self-intersect and leave pinholes in a/g at this weight.
            // Runs are cached, so this work is paid only when text/scale changes.
            draw_glyph(&glyph->bitmap, pen_x + glyph->bitmap_left,
                       baseline - glyph->bitmap_top, pass == 0);
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
static _Thread_local text_cache_entry g_text_cache[TEXT_CACHE_COUNT];
static _Thread_local size_t g_text_cache_bytes;
static _Thread_local uint64_t g_text_cache_clock, g_text_texture_id;

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
    double profile_start=text_profile_ms();
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
    g_cached_outline_pass=1;
    draw_text_uncached(entry->text, entry->pixels,
                       entry->advance / 2 - entry->left,
                       -entry->top - entry->baseline_shift);
    g_cached_outline_pass=0;
    g_pixels = frame;
    g_width = frame_width;
    g_height = frame_height;
    if(!entry->texture_id)entry->texture_id = ++g_text_texture_id;
    entry->rasterized = 1;
    if(getenv("TAIKO_TEXT_PROFILE")) fprintf(stderr,"[TEXT] horizontal px=%d ms=%.3f text=%s\n",entry->pixels,text_profile_ms()-profile_start,entry->text);
    return 1;
}

static int text_width(const char* text, int pixels)
{
    text_cache_entry* entry = get_text(text, pixels);
    return entry ? entry->advance : text_width_uncached(text, pixels);
}

/* CPU-only worker owns its FreeType face and raster state. The renderer
 * only submits keys and reads completed immutable buffers; it never waits for
 * glyph generation. Jobs are bounded and newly visible labels take priority. */
enum { ASYNC_TEXT_SLOTS=96 };
typedef struct AsyncText {
    int state, spine;
    unsigned rgb, scale;
    double used, ready;
    text_cache_entry result;
} AsyncText;
static AsyncText g_async_text[ASYNC_TEXT_SLOTS];
static pthread_mutex_t g_async_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_async_cond=PTHREAD_COND_INITIALIZER;
static pthread_once_t g_async_once=PTHREAD_ONCE_INIT;
static int g_async_available;
static uint64_t g_async_texture_id=UINT64_C(0x4900000000000000);
static void *text_worker(void *unused) {
    (void)unused;
    font_ready();
    for(;;) {
        pthread_mutex_lock(&g_async_lock);
        int selected=-1;
        for(int i=0;i<ASYNC_TEXT_SLOTS;++i)
            if(g_async_text[i].state==1 && (selected<0 || g_async_text[i].used>g_async_text[selected].used))selected=i;
        if(selected<0) {pthread_cond_wait(&g_async_cond,&g_async_lock);pthread_mutex_unlock(&g_async_lock);continue;}
        AsyncText *job=&g_async_text[selected];job->state=2;
        text_cache_entry result=job->result;
        int spine=job->spine;unsigned scale=job->scale,rgb=job->rgb;
        pthread_mutex_unlock(&g_async_lock);
        if(spine) {
            result.width=56*scale;result.height=400*scale;
            result.bitmap=calloc((size_t)result.width*result.height,4);
            result.rasterized=result.bitmap && taiko_title_render_spine_scaled_argb(result.text,result.bitmap,rgb,scale);
            if(result.rasterized)for(int i=0;i<result.width*result.height;++i) {
                uint32_t c=result.bitmap[i];result.bitmap[i]=(c&0xff00ff00u)|((c>>16)&255)|((c&255)<<16);
            }
        } else {
            g_outline_radius=result.outline;
            rasterize_text(&result);
            // Ownership transfers to the job cache, not the worker's cache.
            g_text_cache_bytes=0;
        }
        pthread_mutex_lock(&g_async_lock);
        job->result=result;job->ready=text_profile_ms();job->state=3;
        pthread_mutex_unlock(&g_async_lock);
        wake_renderer();
    }
    return NULL;
}
static void start_text_worker(void) {
    pthread_t thread;
    if(!pthread_create(&thread,NULL,text_worker,NULL)) {
        pthread_detach(thread);g_async_available=1;
    }
}
static text_cache_entry *async_text(const text_cache_entry *key,int spine,unsigned scale,unsigned rgb,float *fade) {
    pthread_once(&g_async_once,start_text_worker);
    if(!g_async_available)return NULL;
    double now=text_profile_ms();
    pthread_mutex_lock(&g_async_lock);
    int slot=-1;
    for(int i=0;i<ASYNC_TEXT_SLOTS;++i) {
        AsyncText *job=&g_async_text[i];
        if(job->state && job->spine==spine && job->scale==scale && job->rgb==rgb &&
           job->result.pixels==key->pixels && job->result.outline==key->outline && !strcmp(job->result.text,key->text)) {
            job->used=now;
            text_cache_entry *result=job->state==3 && job->result.rasterized?&job->result:NULL;
            *fade=(float)((now-job->ready)/80.0);if(*fade>1)*fade=1;if(*fade<0)*fade=0;
            pthread_mutex_unlock(&g_async_lock);return result;
        }
        if(!job->state || (job->state!=2 && now-job->used>1000 && (slot<0 || job->used<g_async_text[slot].used)))slot=i;
    }
    if(slot>=0) {
        AsyncText *job=&g_async_text[slot];free(job->result.bitmap);memset(job,0,sizeof(*job));
        job->result=*key;job->result.bitmap=NULL;job->result.rasterized=0;
        job->result.texture_id=++g_async_texture_id;
        job->spine=spine;job->scale=scale;job->rgb=rgb;job->used=now;job->state=1;
        pthread_cond_signal(&g_async_cond);
    }
    pthread_mutex_unlock(&g_async_lock);return NULL;
}

static void draw_text_at(const char* text, int pixels, float centre_x, float centre_y)
{
    if (g_ui_emit) {
        const int native_pixels = (int)ceilf(pixels * g_ui_scale);
        g_outline_radius = (int)ceilf(g_menu_text_outline * g_ui_scale);
        text_cache_entry* native = get_text(text, native_pixels);
        float fade=1;
        if(native)native=async_text(native,0,0,0,&fade);
        if (native) {
            HostUiDraw draw = {0};
            draw.x = centre_x + (native->left - native->advance / 2) / g_ui_scale;
            draw.y = centre_y + (native->top + native->baseline_shift) / g_ui_scale;
            draw.w = native->width / g_ui_scale;
            draw.h = native->height / g_ui_scale;
            draw.colour = ((unsigned)(g_text_opacity*fade)<<24)|0xffffffu;
            draw.texture_id = native->texture_id;
            draw.pixels = native->bitmap;
            draw.width = native->width;
            draw.height = native->height;
            g_ui_emit(g_ui_user, &draw);
        }
        g_outline_radius = TEXT_OUTLINE_RADIUS;
        return;
    }
    g_outline_radius = g_menu_text_outline;
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
            if ((colour >> 24) == 255 && g_text_opacity==255)
                g_pixels[(size_t)(top + y) * g_width + left + x] = colour;
            else if (colour >> 24)
                put_pixel(left + x, top + y, colour, g_text_opacity);
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
            put_pixel(left + x, top + y, colour, g_text_opacity);
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


static void emit_portrait(unsigned slot, float slide, unsigned alpha)
{
    if (!g_ui_emit || !(g_browser_joined & (1u << slot)) ||
        !g_portraits[slot].address) return;
    HostUiDraw portrait = {0};
    /* Entry camera includes transparent padding around the model. */
    portrait.x = 28 + 150 + slide;
    portrait.y = 100 + slot * 272 - 75;
    portrait.w = portrait.h = 450;
    if (green_categories()) {
        portrait.x = (slot ? 845 : -185) + slide;
        portrait.y = 190;
        portrait.w = portrait.h = 620;
    }
    portrait.colour = (alpha << 24) | 0xffffffu;
    portrait.surface_address = g_portraits[slot].address;
    portrait.width = g_portraits[slot].width;
    portrait.height = g_portraits[slot].height;
    portrait.flip_x = slot == 1 && !green_categories();
    g_ui_emit(g_ui_user, &portrait);
}


#include "taiko_menu_layout.h"

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

    if (green_categories()) {
        g_menu_text_outline=4;
        render_green_categories();
        g_menu_text_outline=TEXT_OUTLINE_RADIUS;
        g_outline_radius=TEXT_OUTLINE_RADIUS;
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

    draw_text_left_fit("SONG SELECT", 39, 490, 34, 49);
    /* Player panels fill the column below the heading. */

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
        snprintf(result_count, sizeof(result_count), "%u %s",
                 g_song_match_total, g_song_match_total == 1 ? "SONG" : "SONGS");
    draw_text_right(result_count, 18, 1224, 58);

    if (!g_song_catalog_total) {
        draw_text_at("CATALOG UNAVAILABLE", 42, 900, 320);
        draw_text_at("CHECK PS3_VFS_ROOT AND MUSICINFO.XML", 23, 900, 375);
        return;
    }

    if (!g_song_row_count &&
        g_song_browser_level == TAIKO_OVERLAY_BROWSER_SONGS) {
        draw_text_at("NO MATCHES", 43, 910, 313);
        draw_text_at("BACKSPACE TO EDIT OR ESC TO CLEAR", 22, 910, 369);
    } else {
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
                draw_text_left_fit(item->title, 22, 1032 - left - 110, left + 18, top + 27);
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
                        ? "RETURN TO CATEGORIES" : item->genre,
                    15, 350, left + 19, top + 40);
            }
            char number[24];
            if (item->kind == TAIKO_OVERLAY_ROW_CATEGORY)
                snprintf(number, sizeof(number), "%u %s",
                         item->catalog_index, item->catalog_index == 1 ? "SONG" : "SONGS");
            else if (item->kind == TAIKO_OVERLAY_ROW_EXIT)
                snprintf(number, sizeof(number), "EXIT");
            else
                snprintf(number, sizeof(number), "%03u",
                         item->catalog_index + 1);
            draw_text_right(number, 17, 1231, top + 27);
        }
    }

    if (g_browser_players_enabled) {
        static const char* courses[] = {"EASY", "NORMAL", "HARD", "ONI", "URA"};
        /* Keep avatar and future settings space within each player panel,
         * rather than reserving an empty area above both players. */
        for (unsigned slot = 0; slot < 2; ++slot) {
            const int left = 28;
            const int top = 100 + (int)slot * 272;
            const int joined = (g_browser_joined & (1u << slot)) != 0;
            const uint32_t colour = slot ? RGB_COLOUR(0x32, 0x80, 0xAC)
                                         : RGB_COLOUR(0xB6, 0x46, 0x55);
            fill_rounded_rect(left, top, left + 509, top + 256, 12,
                              RGB_COLOUR(0x29, 0x39, 0x49));
            emit_portrait(slot, 0, 255);
            fill_rounded_rect(left + 14, top + 14, left + 63, top + 57, 9, colour);
            char badge[8];
            snprintf(badge, sizeof badge, "P%u", slot + 1);
            draw_text_at(badge, 22, left + 38, top + 36);
            draw_text_left_fit((g_browser_authenticated & (1u << slot))
                                   ? g_browser_account_names[slot]
                                   : joined ? "GUEST" : "NOT JOINED",
                               21, 414, left + 77, top + 36);
            char participation[48];
            snprintf(participation, sizeof participation, "%u  %s", slot + 1,
                     joined ? "LEAVE PLAYER" : "JOIN PLAYER");
            draw_text_left_fit(participation, 16, 460, left + 16, top + 230);
            if (!joined && (g_browser_authenticated & (1u << slot)))
                draw_text_right("NOT JOINED", 15, left + 493, top + 82);
            if (joined) {
                const unsigned course = g_browser_difficulties[slot];
                draw_text_left_fit(course < 5 ? courses[course] : "CHOOSE CHART",
                                   17, 130, left + 16, top + 82);
                if (g_browser_ready & (1u << slot))
                    draw_text_right("READY", 15, left + 493, top + 82);
            } else {
                draw_text_left_fit("HIT DRUM TO JOIN", 16, 216, left + 15, top + 82);
            }
        }
    }

    if (g_browser_login_phase) {
        fill_rect(540, 230, 1260, 490, RGB_COLOUR(0x13, 0x22, 0x34));
        draw_text_at("BANAPASSPORT", 28, 900, 265);
        draw_text_at(g_browser_login_status, 21, 900, 310);
        if (g_browser_login_phase == 1) {
            draw_text_at(g_code[0] ? g_code : "------", 56, 900, 375);
            draw_text_at("Enter this PIN on the pairing website", 19, 900, 428);
        }
        draw_text_at("ESC  CANCEL", 16, 900, 467);
    } else {
        draw_text_left_fit("B  BANAPASSPORT LOGIN", 16, 470, 40, 641);
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
        if (g_browser_players_enabled) {
            if (green_categories() && panel < 2)
                emit_portrait(panel, (float)(x-xs[panel]), alpha);
            else if (!green_categories() && panel == 0) {
                emit_portrait(0, (float)x, alpha);
                emit_portrait(1, (float)x, alpha);
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
    if (g_mode >= 3 && g_mode != 4 && !(g_mode == 5 && g_browser_login_phase == 1)) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    snprintf(g_code, sizeof(g_code), "%s", code ? code : "");
    g_deadline = monotonic_seconds() + (expires_in > 0 ? expires_in : 0);
    g_visible = g_code[0] != '\0';
    if (g_mode < 3) g_mode = 1;
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

void taiko_overlay_set_browser_login(int phase, const char* status)
{
    pthread_mutex_lock(&g_lock);
    if (g_browser_login_phase != phase || strcmp(g_browser_login_status, status ? status : "")) {
        if (g_browser_login_phase != phase) g_code[0] = 0;
        g_browser_login_phase = phase;
        snprintf(g_browser_login_status, sizeof g_browser_login_status, "%s", status ? status : "");
        g_drawn_remaining = -1;
        ++g_version;
    }
    pthread_mutex_unlock(&g_lock);
    wake_renderer();
}

void taiko_overlay_set_browser_account(unsigned slot, const char* name, int authenticated)
{
    if (slot > 1) return;
    pthread_mutex_lock(&g_lock);
    snprintf(g_browser_account_names[slot], sizeof g_browser_account_names[slot],
             "%s", name && name[0] ? name : "BanaPassport player");
    if (authenticated) g_browser_authenticated |= 1u << slot;
    else g_browser_authenticated &= ~(1u << slot);
    ++g_version;
    pthread_mutex_unlock(&g_lock);
    wake_renderer();
}

uint8_t taiko_overlay_browser_joined(void)
{
    pthread_mutex_lock(&g_lock);
    const uint8_t joined = g_browser_players_enabled ? g_browser_joined : 0;
    pthread_mutex_unlock(&g_lock);
    return joined;
}

int taiko_overlay_browser_visible(void)
{
    pthread_mutex_lock(&g_lock);
    finish_handoff_if_due();
    const int visible = g_visible && g_mode == 5;
    pthread_mutex_unlock(&g_lock);
    return visible;
}

void taiko_overlay_set_browser_portrait(unsigned player, uint32_t address,
                                        uint32_t width, uint32_t height)
{
    if (player >= 2) return;
    pthread_mutex_lock(&g_lock);
    if (g_portraits[player].address != address ||
        g_portraits[player].width != width || g_portraits[player].height != height) {
        g_portraits[player].address = address;
        g_portraits[player].width = width;
        g_portraits[player].height = height;
        ++g_version;
    }
    pthread_mutex_unlock(&g_lock);
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
    const int was_categories = g_mode == 5 && g_visible &&
        g_song_browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES;
    unsigned old_selected=0, incoming_selected=0;
    for(unsigned i=0;i<g_song_row_count;++i)if(g_song_rows[i].selected)old_selected=i;
    for(unsigned i=0;rows && i<row_count && i<TAIKO_OVERLAY_SONG_ROW_COUNT;++i)
        if(rows[i].selected)incoming_selected=i;
    const int shared=rows && row_count && rows[0].carousel_group!=0;
    int opening_folder = was_categories && browser_level != TAIKO_OVERLAY_BROWSER_CATEGORIES &&
        !search_active && (!query || !*query) && rows && row_count;
    if(shared) opening_folder=opening_folder &&
        g_song_rows[old_selected].carousel_group==rows[incoming_selected].carousel_group;
    for (unsigned i=0; opening_folder && i<row_count && i<TAIKO_OVERLAY_SONG_ROW_COUNT; ++i)
        if ((!shared && rows[i].kind == TAIKO_OVERLAY_ROW_CATEGORY) ||
            rows[i].kind == TAIKO_OVERLAY_ROW_DIFFICULTY) opening_folder=0;
    const int closing_folder = g_folder_open && !was_categories && g_visible &&
        browser_level==TAIKO_OVERLAY_BROWSER_CATEGORIES &&
        title && !strcmp(title,g_song_category);
    if(closing_folder) {
        memcpy(g_folder_close_rows,g_song_rows,sizeof g_folder_close_rows);
        g_folder_close_count=g_song_row_count;
        snprintf(g_folder_close_category,sizeof g_folder_close_category,"%s",g_song_category);
        g_folder_close_difficulties=g_song_difficulty_mask;
        g_folder_close_left=menu_folder_left();
        g_folder_close_right=menu_folder_right();
        g_folder_close_group=g_song_rows[old_selected].carousel_group;
        if(g_song_shared_carousel) menu_shared_bounds(&g_song_rows[old_selected],0,song_ease(),
            &g_folder_close_left,&g_folder_close_right);
        g_folder_close_start=monotonic_milliseconds();
        g_folder_closing=1;
    } else if(opening_folder || !g_visible || g_mode!=5 || search_active ||
              (query && *query) || (g_folder_closing && title &&
              strcmp(title,g_folder_close_category))) g_folder_closing=0;
    if (opening_folder) {
        memcpy(g_folder_categories,g_song_rows,sizeof g_folder_categories);
        g_folder_category_count=g_song_row_count;
        g_folder_category_selected=0;
        for(unsigned i=0;i<g_song_row_count;++i)
            if(g_song_rows[i].selected) g_folder_category_selected=(int)i;
        g_folder_open_start=monotonic_milliseconds();
        g_folder_open=1;
        g_folder_scroll_from=g_folder_scroll_target=0;
    } else if (browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES ||
               g_mode != 5 || !g_visible || search_active || (query && *query) ||
               (category && strcmp(category,g_song_category))) {
        g_folder_open=0;
        if(!g_folder_closing) g_folder_category_count=0;
    }
    if(shared && !opening_folder && browser_level==TAIKO_OVERLAY_BROWSER_SONGS &&
       rows[incoming_selected].kind!=TAIKO_OVERLAY_ROW_CATEGORY &&
       rows[incoming_selected].kind!=TAIKO_OVERLAY_ROW_DIFFICULTY && !g_folder_open) {
        // Entering an already open neighbour is a scroll, never an opening.
        g_folder_open=1;g_folder_open_start=monotonic_milliseconds()-1000;
        g_folder_scroll_from=g_folder_scroll_target=rows[incoming_selected].browser_position*96.0f;
    }
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
    int previous_selected=0, next_selected=0;
    for(unsigned i=0;i<previous_count;++i) if(previous[i].selected) previous_selected=(int)i;
    for(unsigned i=0;rows && i<row_count && i<TAIKO_OVERLAY_SONG_ROW_COUNT;++i) if(rows[i].selected) next_selected=(int)i;
    int changed = previous_count != row_count;
    int card_shift=0, have_card_shift=0;
    unsigned unmatched_cards=0;
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
        item->browser_position=rows?rows[row].browser_position:0;
        item->browser_total=rows?rows[row].browser_total:0;
        item->carousel_group=rows?rows[row].carousel_group:0;
        item->difficulty = rows ? rows[row].difficulty : 0;
        item->stars = rows ? rows[row].stars : 0;
        item->cursors = rows ? rows[row].cursors : 0;
        item->ready = rows ? rows[row].ready : 0;
        for(unsigned d=0;d<5;++d) item->course_stars[d]=rows?rows[row].course_stars[d]:0;
        item->from_y = 111 + row * 59;
        item->from_x = row_target_x(item->kind, item->selected) + 36;
        int relative=(int)row-next_selected;
        item->from_card_x=menu_card_x(relative);
        item->from_card_w=menu_card_w(relative);
        menu_shared_target(item,relative,&item->from_group_left,&item->from_group_right);
        int found = -1;
        for (unsigned old = 0; old < previous_count; ++old) {
            const song_row_storage* prior = &previous[old];
            if (prior->carousel_group==item->carousel_group &&
                prior->kind == item->kind && prior->catalog_index == item->catalog_index &&
                prior->difficulty == item->difficulty && !strcmp(prior->title, item->title)) {
                found = (int)old;
                item->from_y = prior->from_y + (111 + old * 59 - prior->from_y) * old_ease;
                item->from_x = prior->from_x + (row_target_x(prior->kind, prior->selected) - prior->from_x) * old_ease;
                int prior_relative=(int)old-previous_selected;
                if(!have_card_shift) {
                    card_shift=prior_relative-relative;
                    have_card_shift=1;
                }
                menu_card_pose(prior->from_card_x,prior->from_card_w,prior_relative,old_ease,
                               &item->from_card_x,&item->from_card_w);
                menu_shared_bounds(prior,prior_relative,old_ease,&item->from_group_left,&item->from_group_right);
                changed |= old != row || prior->selected != item->selected ||
                           prior->cursors != item->cursors || prior->ready != item->ready;
                break;
            }
        }
        changed |= found < 0;
        if(found<0) unmatched_cards|=1u<<row;
    }
    /* Newly exposed cards enter from beyond the screen edge. Starting them
     * at their destination made departing neighbours slide across them. */
    if(have_card_shift && card_shift) for(unsigned row=0;row<g_song_row_count;++row) {
        if(!(unmatched_cards&(1u<<row))) continue;
        int relative=(int)row-next_selected+card_shift;
        g_song_rows[row].from_card_x=menu_card_x(relative);
        g_song_rows[row].from_card_w=menu_card_w(relative);
        menu_shared_target(&g_song_rows[row],relative,&g_song_rows[row].from_group_left,&g_song_rows[row].from_group_right);
    }
    g_song_shared_carousel=shared;
    for(unsigned i=0;i<g_song_row_count;++i)
        if(g_song_rows[i].kind==TAIKO_OVERLAY_ROW_DIFFICULTY)g_song_shared_carousel=0;
    if(closing_folder && shared) {
        memcpy(g_folder_categories,g_song_rows,sizeof g_folder_categories);
        g_folder_category_count=g_song_row_count;g_folder_category_selected=next_selected;
    }
    if(g_folder_open && green_categories() && g_song_row_count) {
        float target=g_song_rows[next_selected].browser_position*96.0f;
        if(target!=g_folder_scroll_target) {
            g_folder_scroll_from=menu_folder_scroll();
            g_folder_scroll_target=target;
            g_folder_scroll_start=monotonic_milliseconds();
        }
    }
    if (changed) {
        g_song_animation_start = monotonic_milliseconds();
        /* Navigation interrupts, rather than queues behind, an unfinished
         * carousel animation. Land on the next closed spine immediately and
         * retain the normal idle delay before opening it. */
        if(green_categories() && !g_song_shared_carousel && !g_folder_open && have_card_shift && card_shift && old_ease<1.0f)
            g_song_animation_start -= 8000.0/60;
        g_song_animating = 1;
        g_gpu_animation_pending = 1;
    } else {
        /* Repeated publications (including held input) must not restart easing. */
        for (unsigned row = 0; row < g_song_row_count; ++row) {
            g_song_rows[row].from_card_x = previous[row].from_card_x;
            g_song_rows[row].from_card_w = previous[row].from_card_w;
            g_song_rows[row].from_group_left=previous[row].from_group_left;
            g_song_rows[row].from_group_right=previous[row].from_group_right;
            g_song_rows[row].from_y = previous[row].from_y;
            g_song_rows[row].from_x = previous[row].from_x;
        }
    }
    if(closing_folder) g_song_animation_start=monotonic_milliseconds()-500;
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
    if ((g_mode == 4 || (g_mode == 5 && g_browser_login_phase == 1)) && g_code[0] && remaining == 0) {
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
        (g_mode == 5 && (g_song_animating || g_handoff || green_categories()) && now - g_song_last_render >= 16.0)) {
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
    if ((g_mode == 4 || (g_mode == 5 && g_browser_login_phase == 1)) && g_code[0] && monotonic_seconds() >= g_deadline) {
        g_code[0] = '\0'; ++g_version; g_drawn_remaining = -1;
    }
    info->version = g_version;
    info->animated = g_mode == 5 && (g_gpu_animation_pending || g_handoff);
    if (green_categories() && g_browser_players_enabled) info->animated = 1;
    if (g_mode == 5 && g_browser_players_enabled &&
        ((g_portraits[0].address && (g_browser_joined & 1)) ||
         (g_portraits[1].address && (g_browser_joined & 2)))) info->animated = 1;
    pthread_mutex_lock(&g_async_lock);
    for(int i=0;i<ASYNC_TEXT_SLOTS;++i)if(g_async_text[i].state && text_profile_ms()-g_async_text[i].used<200 && (g_async_text[i].state!=3 || text_profile_ms()-g_async_text[i].ready<100))info->animated=1;
    pthread_mutex_unlock(&g_async_lock);
    info->overlay = g_handoff < 0;
    if ((g_mode == 4 || (g_mode == 5 && g_browser_login_phase == 1)) && g_code[0]) info->animated = 1;
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
