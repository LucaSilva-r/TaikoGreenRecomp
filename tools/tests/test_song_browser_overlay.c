/* Rasterize one representative host Song Select frame without booting Green.
 * This is both a smoke test for the overlay API and a quick visual fixture.
 *
 *   cc -DTAIKO_OVERLAY_FONT_EMBEDDED tools/tests/test_song_browser_overlay.c \
 *      src/taiko_overlay.c build-linux/taiko_overlay_font.c -I src \
 *      ps3recomp/libs/video/rsx_render_batch.c \
 *      -I ps3recomp/libs/video -I ps3recomp/include \
 *      -I third_party/freetype-linux/include/freetype2 \
 *      third_party/freetype-linux/lib64/libfreetype.a -pthread -lz -lm \
 *      -o /tmp/song-browser-overlay-test
 *   /tmp/song-browser-overlay-test /tmp/song-browser-overlay.ppm
 */
#include "taiko_overlay.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void write_frame(const char* output)
{
    HostFrameInfo info = {0};
    assert(taiko_host_frame_copy(&info, NULL, 0));
    assert(info.mode == HOST_FRAME_FULLSCREEN &&
           info.width == 1280 && info.height == 720 && info.version != 0);
    const size_t bytes = (size_t)info.pitch * info.height;
    uint32_t* pixels = (uint32_t*)malloc(bytes);
    assert(pixels);
    assert(taiko_host_frame_copy(&info, pixels, bytes));

    FILE* file = fopen(output, "wb");
    assert(file);
    fprintf(file, "P6\n%u %u\n255\n", info.width, info.height);
    for (uint32_t index = 0; index < info.width * info.height; ++index) {
        const unsigned char rgb[3] = {
            (unsigned char)(pixels[index] & 0xffu),
            (unsigned char)((pixels[index] >> 8) & 0xffu),
            (unsigned char)((pixels[index] >> 16) & 0xffu),
        };
        assert(fwrite(rgb, sizeof(rgb), 1, file) == 1);
    }
    assert(fclose(file) == 0);
    printf("song browser overlay ok: %ux%u version %u -> %s\n",
           info.width, info.height, info.version, output);
    free(pixels);
}

int main(int argc, char** argv)
{
    const char* output = argc > 1 ? argv[1] :
        "/tmp/song-browser-overlay.ppm";
    const taiko_overlay_song_row rows[TAIKO_OVERLAY_SONG_ROW_COUNT] = {
        {"Blue Moon", "VOCALOID", 10, 0, TAIKO_OVERLAY_ROW_SONG},
        {"Night of Knights", "VOCALOID", 11, 0, TAIKO_OVERLAY_ROW_SONG},
        {"Senbonzakura", "VOCALOID", 12, 0, TAIKO_OVERLAY_ROW_SONG},
        {"Bad Apple!! feat. nomico", "VOCALOID", 13, 0,
         TAIKO_OVERLAY_ROW_SONG},
        {"World is Mine", "VOCALOID", 14, 1, TAIKO_OVERLAY_ROW_SONG},
        {"Tell Your World", "VOCALOID", 15, 0, TAIKO_OVERLAY_ROW_SONG},
        {"Melt", "VOCALOID", 16, 0, TAIKO_OVERLAY_ROW_SONG},
        {"The Disappearance of Hatsune Miku", "VOCALOID", 17, 0,
         TAIKO_OVERLAY_ROW_SONG},
        {"BACK TO CATEGORIES", "VOCALOID", 1, 0,
         TAIKO_OVERLAY_ROW_EXIT},
    };

    const uint8_t difficulties[2] = {3, 2};
    taiko_overlay_set_browser_players(1, 3, 1, difficulties);
    taiko_overlay_set_browser_account(0, "Player One", 1);
    taiko_overlay_show_song_browser(
        "ANONYMOUS - OFFLINE", "mikugv", "World is Mine", "VOCALOID",
        875, 14, 37, 853, "VOCALOID", 2, 9, "ONI", 0x1f, "miku", 1,
        TAIKO_OVERLAY_BROWSER_SONGS, 0, rows,
        TAIKO_OVERLAY_SONG_ROW_COUNT);

    write_frame(output);

    if (argc > 2) {
        const taiko_overlay_song_row categories[] = {
            {"J-POP", "J-POP", 84, 0, TAIKO_OVERLAY_ROW_CATEGORY},
            {"ANIME", "ANIME", 85, 0, TAIKO_OVERLAY_ROW_CATEGORY},
            {"VOCALOID", "VOCALOID", 47, 1, TAIKO_OVERLAY_ROW_CATEGORY},
            {"VARIETY", "VARIETY", 54, 0, TAIKO_OVERLAY_ROW_CATEGORY},
            {"CLASSICAL", "CLASSICAL", 27, 0,
             TAIKO_OVERLAY_ROW_CATEGORY},
            {"GAME MUSIC", "GAME MUSIC", 152, 0,
             TAIKO_OVERLAY_ROW_CATEGORY},
            {"NAMCO ORIGINAL", "NAMCO ORIGINAL", 347, 0,
             TAIKO_OVERLAY_ROW_CATEGORY},
            {"MEDLEY", "MEDLEY", 53, 0, TAIKO_OVERLAY_ROW_CATEGORY},
            {"CHILDREN'S SONGS", "CHILDREN'S SONGS", 4, 0,
             TAIKO_OVERLAY_ROW_CATEGORY},
        };
        taiko_overlay_show_song_browser(
            "ANONYMOUS - OFFLINE", "", "VOCALOID", "CATEGORY FOLDER",
            47, 2, 9, 853, "CATEGORIES", 2, 9, "", 0, "", 0,
            TAIKO_OVERLAY_BROWSER_CATEGORIES, 0, categories, 9);
        write_frame(argv[2]);
    }
    return 0;
}
