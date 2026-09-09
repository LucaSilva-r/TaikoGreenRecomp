#include "taiko_title_render.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(int argc, char** argv)
{
    // Exercise calibrated horizontal profiles with Japanese glyphs, Latin
    // spacing, subtitles, and long-title downscaling. Inspect alpha as well as
    // colour: an ARGB remap error otherwise makes a valid title disappear.
    uint32_t pixels[720 * 104];
    const char* titles[] = {"テスト曲", "Custom integration test",
        "A very long custom song title that must fit inside the native texture without clipping"};
    for (unsigned type = 11; type <= 12; ++type) {
        unsigned w, h;
        CHECK(title_tex_dims(type, &w, &h));
        for (unsigned t = 0; t < 3; ++t) {
            memset(pixels, 0, sizeof(pixels));
            CHECK(title_tex_render_ex(type, titles[t], "サブタイトル", pixels, w, h, 0));
            unsigned fill = 0, outline = 0, transparent = 0;
            for (unsigned i = 0; i < w * h; ++i) {
                if (!(pixels[i] >> 24)) ++transparent;
                else if ((pixels[i] & 0xffffff) == 0xffffff) ++fill;
                else if (!(pixels[i] & 0xffffff)) ++outline;
            }
            // Both ends must remain inside the texture, including long titles.
            for (unsigned y = 0; y < h; ++y) {
                CHECK((pixels[y*w] >> 24) == 0);
                CHECK((pixels[y*w+w-1] >> 24) == 0);
            }
            CHECK(fill > 100 && outline > 100 && transparent > w * h / 4);
            if (argc > 1 && type == 12 && t == 0) {
                FILE* out = fopen(argv[1], "wb");
                CHECK(out);
                CHECK(fwrite(pixels, 4, w * h, out) == w * h);
                fclose(out);
            }
        }
    }
    return 0;
}
