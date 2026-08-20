/* Does the embedded, subset overlay font still rasterize?
 *
 * The subset drops everything but "0123456789-", so the failure mode to catch
 * is a font that loads yet renders blank boxes for the pairing code.
 *
 *   cc tools/tests/test_overlay_font.c build-linux/taiko_overlay_font.c \
 *      -I third_party/freetype-linux/include/freetype2 \
 *      third_party/freetype-linux/lib64/libfreetype.a -lz -lm -o /tmp/fonttest
 *   /tmp/fonttest
 */
#include <assert.h>
#include <stdio.h>

#include <ft2build.h>
#include FT_FREETYPE_H

extern const unsigned char taiko_overlay_font_data[];
extern const unsigned taiko_overlay_font_size;

int main(void)
{
    FT_Library library;
    FT_Face face;

    assert(FT_Init_FreeType(&library) == 0);
    assert(FT_New_Memory_Face(library, taiko_overlay_font_data,
                              (FT_Long)taiko_overlay_font_size, 0, &face) == 0);
    assert(FT_Set_Pixel_Sizes(face, 0, 48) == 0);

    for (const char* c = "0123456789-"; *c; c++) {
        assert(FT_Get_Char_Index(face, (FT_ULong)*c) != 0);
        assert(FT_Load_Char(face, (FT_ULong)*c, FT_LOAD_RENDER) == 0);

        const FT_Bitmap* bitmap = &face->glyph->bitmap;
        assert(bitmap->width > 0 && bitmap->rows > 0);

        unsigned ink = 0;
        for (unsigned y = 0; y < bitmap->rows; y++)
            for (unsigned x = 0; x < bitmap->width; x++)
                ink += bitmap->buffer[y * (unsigned)bitmap->pitch + x] != 0;
        assert(ink > 0);
    }

    printf("embedded font ok: %u bytes, '%s', 11 glyphs rasterize\n",
           taiko_overlay_font_size,
           face->family_name ? face->family_name : "?");
    return 0;
}
