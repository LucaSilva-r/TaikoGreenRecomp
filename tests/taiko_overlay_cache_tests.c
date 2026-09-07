/* Test the real rasterizer/cache without exposing cache controls in the game. */
#include "../src/taiko_overlay.c"
#undef NDEBUG
#include <assert.h>

static uint32_t reference[HOST_WIDTH * HOST_HEIGHT];

static void background(uint32_t colour)
{
    g_width = HOST_WIDTH;
    g_height = HOST_HEIGHT;
    for (unsigned i = 0; i < HOST_WIDTH * HOST_HEIGHT; ++i) g_pixels[i] = colour;
}

static uint64_t native_ids[512];
static unsigned native_count, native_texts, native_height;
static HostUiDraw native_draws[512];
static void collect_ui(void* user, const HostUiDraw* draw)
{
    (void)user;
    assert(native_count < 512);
    native_draws[native_count] = *draw;
    native_ids[native_count++] = draw->texture_id;
    if (draw->texture_id) {
        assert(draw->pixels && draw->width && draw->height);
        ++native_texts;
        if (draw->height > native_height) native_height = draw->height;
    }
}

int main(void)
{
    assert(font_ready());
    const char* texts[] = {"Groove", "★ 10", "P1 OK   P2 <", "太鼓の達人", "jgy (TEST)", "   ", "", "\xe3\x81"};
    const uint32_t colours[] = {RGB_COLOUR(0x29, 0x3a, 0x4d), 0, 0xffffffffu};
    for (unsigned sample = 0; sample < sizeof(texts) / sizeof(texts[0]); ++sample) {
        for (unsigned c = 0; c < sizeof(colours) / sizeof(colours[0]); ++c) {
            // Check normal placement and clipping against both framebuffer edges.
            for (unsigned edge = 0; edge < 3; ++edge) {
                const int x = edge == 0 ? 250 : edge == 1 ? 0 : HOST_WIDTH;
                const int y = edge == 0 ? 150 : edge == 1 ? 0 : HOST_HEIGHT;
                const int size = sample % 2 ? 17 : 39;
                background(colours[c]);
                draw_text_uncached(texts[sample], size, x, y);
                memcpy(reference, g_pixels, sizeof reference);
                background(colours[c]);
                draw_text_at(texts[sample], size, x, y);
                for (unsigned pixel = 0; pixel < HOST_WIDTH * HOST_HEIGHT; ++pixel) {
                    for (unsigned shift = 0; shift < 32; shift += 8) {
                        const int before = (reference[pixel] >> shift) & 255;
                        const int after = (g_pixels[pixel] >> shift) & 255;
                        // A cached run composites once instead of once per
                        // outline sample; permit only small rounding differences.
                        assert(abs(before - after) <= 3);
                    }
                }
                assert(text_width(texts[sample], size) == text_width_uncached(texts[sample], size));
            }
        }
    }
    text_cache_entry* stable = get_text("Stable label", 22);
    assert(rasterize_text(stable));
    const uint32_t* bitmap = stable->bitmap;
    const size_t bytes = g_text_cache_bytes;
    for (unsigned i = 0; i < 100; ++i) {
        draw_text_at("Stable label", 22, 100 + i, 250);
        assert(get_text("Stable label", 22)->bitmap == bitmap);
        assert(g_text_cache_bytes == bytes);
    }
    background(RGB_COLOUR(0x23, 0x37, 0x4b));
    draw_text_at("Stable label", 22, 200, 200);
    memcpy(reference, g_pixels, sizeof reference);
    // Exceed both the entry count and byte budget; returning to a song must
    // regenerate the same text, and retained storage must remain bounded.
    for (unsigned i = 0; i < TEXT_CACHE_COUNT + 40; ++i) {
        char label[160];
        snprintf(label, sizeof label, "%03u A long song title with many characters - repeated to exercise bitmap eviction and cache limits", i);
        draw_text_at(label, 32, 640, 350);
        assert(g_text_cache_bytes <= TEXT_CACHE_MAX_BYTES);
    }
    background(RGB_COLOUR(0x23, 0x37, 0x4b));
    draw_text_at("Stable label", 22, 200, 200);
    assert(!memcmp(reference, g_pixels, sizeof reference));
    taiko_overlay_show_entry_menu(0);
    HostUiInfo info;
    assert(visit_host_ui(1, collect_ui, NULL, &info));
    assert(native_count && native_texts);
    unsigned first_count = native_count, first_height = native_height;
    uint64_t first_ids[512]; memcpy(first_ids, native_ids, sizeof first_ids);
    native_count = native_texts = native_height = 0;
    assert(visit_host_ui(1, collect_ui, NULL, &info));
    assert(native_count == first_count && !memcmp(first_ids, native_ids, first_count*sizeof(uint64_t)));
    native_count = native_texts = native_height = 0;
    assert(visit_host_ui(3, collect_ui, NULL, &info));
    assert(native_count == first_count && native_height > first_height * 2);
    taiko_overlay_song_row row = {0};
    row.title = "One song"; row.genre = "J-POP"; row.selected = 1;
    taiko_overlay_show_song_browser("P1", "fixture", "One song", "J-POP", 1,
        0, 1, 1, "J-POP", 0, 9, "ONI", 8, "", 0, 1, 0, &row, 1);
    assert(visit_host_ui(1, NULL, NULL, &info) && info.animated);
    g_song_animation_start = monotonic_milliseconds() - 200;
    // Even if the first poll is after the deadline, present the final position.
    assert(visit_host_ui(1, NULL, NULL, &info) && info.animated);
    native_count = 0;
    assert(visit_host_ui(1, collect_ui, NULL, &info));
    assert(visit_host_ui(1, NULL, NULL, &info) && !info.animated);
    // Incoming panels are opaque as a screen, with a black backing. Polling
    // alone must not spend the animation while the guest is busy loading.
    taiko_overlay_animate_browser(0);
    assert(visit_host_ui(1, NULL, NULL, &info) && info.animated && !info.overlay);
    assert(!g_handoff_snapshot);
    native_count = 0;
    assert(visit_host_ui(1, collect_ui, NULL, &info));
    assert(native_count == 4 && native_draws[0].colour == 0xff000000u);
    assert(native_draws[1].x == -96 && native_draws[2].x == 666);
    uint64_t panel_id = native_draws[1].texture_id;
    g_handoff_start = monotonic_milliseconds() - HANDOFF_MS / 2;
    native_count = 0;
    assert(visit_host_ui(1, collect_ui, NULL, &info));
    assert(native_draws[1].texture_id == panel_id); // No animated texture churn.
    assert(native_draws[1].x >= -49 && native_draws[1].x <= -47);
    assert((native_draws[1].colour >> 24) >= 127 && (native_draws[1].colour >> 24) <= 129);
    g_handoff_start = monotonic_milliseconds() - HANDOFF_MS - 1;
    assert(visit_host_ui(1, NULL, NULL, &info) && !info.animated && !info.overlay);

    taiko_overlay_animate_browser(1);
    assert(visit_host_ui(1, NULL, NULL, &info) && info.animated && info.overlay);
    native_count = 0;
    assert(visit_host_ui(1, collect_ui, NULL, &info));
    assert(native_count == 3 && native_draws[0].x == 0);
    g_handoff_start = monotonic_milliseconds() - HANDOFF_MS / 2;
    HostFrameInfo cpu;
    g_song_last_render = 0;
    assert(taiko_host_frame_copy(&cpu, reference, sizeof reference));
    assert(cpu.mode == HOST_FRAME_OVERLAY);
    assert(reference[320 * HOST_WIDTH + 570] == 0); // Opening between panels.
    assert((reference[320 * HOST_WIDTH + 100] >> 24) >= 125);
    assert((reference[320 * HOST_WIDTH + 100] >> 24) <= 129);
    g_handoff_start = monotonic_milliseconds() - HANDOFF_MS - 1;
    assert(!visit_host_ui(1, NULL, NULL, &info));
    assert(!taiko_host_frame_copy(&cpu, NULL, 0) && cpu.mode == HOST_FRAME_NONE);
    taiko_overlay_show_entry_menu(0);
    taiko_overlay_animate_browser(1); // Never animate login/pairing accidentally.
    assert(visit_host_ui(1, NULL, NULL, &info) && !info.overlay);
    taiko_overlay_hide_host_screen();
    assert(!visit_host_ui(3, NULL, NULL, &info));
    for (unsigned i = 0; i < TEXT_CACHE_COUNT; ++i) release_text_bitmap(&g_text_cache[i]);
    assert(g_text_cache_bytes == 0);
    return 0;
}
