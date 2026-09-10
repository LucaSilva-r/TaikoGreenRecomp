/* Green Song Select artwork, read from the user's unmodified Lumen archive.
 * No extracted artwork is embedded or written to disk. Owned RGBA payloads
 * live for the process and use a separate host-UI texture-id namespace. */
#ifndef TAIKO_MENU_ART_H
#define TAIKO_MENU_ART_H
#include "rsx_render_batch.h"

typedef struct menu_art {
    uint32_t* pixels;
    unsigned width, height;
} menu_art;
static menu_art g_menu_art[788];
static int g_menu_art_loaded;

static uint32_t menu_be32(const unsigned char* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Header offsets are parsed, never tied to one dump's archive placement. */
static int menu_archive_table(const unsigned char* h, size_t size,
                              size_t* table, uint32_t* count, size_t* base)
{
    if (size < 20 || memcmp(h, "LM_NUT_TYPE1", 12)) return 0;
    size_t p = 20u + (size_t)menu_be32(h + 16) + 20u;
    if (p > size || size - p < 13) return 0;
    uint32_t lms = menu_be32(h + p); p += 13;
    if (!lms || lms > 1024) return 0;
    for (uint32_t i = 0; i < lms; ++i) {
        if (p > size || size - p < 4) return 0;
        size_t name = menu_be32(h + p); p += 4;
        size_t extra = (i == 0 ? 5 : 0) + 16;
        if (name > size - p || extra > size - p - name) return 0;
        p += name + extra;
    }
    if (p > size || size - p < 18) return 0;
    p += 5; *count = menu_be32(h + p); p += 13;
    if (*count != 788 || (size_t)*count * 8 + 16 > size - p) return 0;
    *table = p; p += (size_t)*count * 8;
    *base = p + 16 + (size_t)menu_be32(h + p);
    return 1;
}

static int menu_decode_nut(const unsigned char* b, size_t size, menu_art* out)
{
    if (size < 96 || memcmp(b, "NTP3", 4) || b[6] || b[7] != 1) return 0;
    unsigned w = (b[36] << 8) | b[37], h = (b[38] << 8) | b[39];
    size_t payload = menu_be32(b + 24);
    if (!w || !h || w > 2048 || h > 2048 || payload > size - 96) return 0;
    const unsigned char* data = b + size - payload;
    uint32_t* pixels = (uint32_t*)malloc((size_t)w * h * 4);
    if (!pixels) return 0;
    int valid = 0;
    if (b[35] == 2)
        valid = rsx_decode_bc_texture(RSX_TEXTURE_BC3, data, payload, 0,
                                      w, h, pixels, (uint64_t)w * h * 4) == 0;
    else if ((b[35] == 14 || b[35] == 17) && payload >= (size_t)w * h * 4) {
        for (size_t i = 0; i < (size_t)w * h; ++i)
            pixels[i] = ((uint32_t)data[i*4] << 24) | data[i*4+1] |
                        ((uint32_t)data[i*4+2] << 8) | ((uint32_t)data[i*4+3] << 16);
        valid = 1;
    }
    if (!valid) { free(pixels); return 0; }
    out->pixels = pixels; out->width = w; out->height = h;
    return 1;
}

static void menu_load_art(void)
{
    if (g_menu_art_loaded) return;
    g_menu_art_loaded = 1;
    const char* root = getenv("PS3_VFS_ROOT");
    if (!root || !*root) root = "game/vfs";
    char path[4096];
    if (snprintf(path, sizeof path, "%s/data/lumendata/packed/song_select/packeddata.ddp", root) >= (int)sizeof path) return;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    unsigned char header[65536];
    size_t read = fread(header, 1, sizeof header, f), table, base;
    uint32_t count;
    if (!menu_archive_table(header, read, &table, &count, &base) ||
        fseek(f, 0, SEEK_END)) { fclose(f); return; }
    long end = ftell(f);
    /* Texture ids from Green's song_select packlist. The border strips and
     * tabs are composed by the layout; dynamic labels remain native text. */
    static const unsigned ids[] = {90,91,244,394,641,643,645,647,649,651,653,655,
        657,659,661,663,667,673,676,
        550,551,556,557,561,562,566,567,571,572,576,577,581,582,586,587,
        591,592,596,597,616,617,
        771,787,87,195,196};
    unsigned loaded = 0;
    size_t retained = 0;
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; ++i) {
        unsigned id = ids[i];
        size_t offset = base + menu_be32(header + table + id * 8);
        size_t size = menu_be32(header + table + id * 8 + 4);
        if (end < 0 || offset > (size_t)end || size > (size_t)end - offset ||
            size < 96 || size > 16 * 1024 * 1024 || offset > LONG_MAX) continue;
        unsigned char* nut = (unsigned char*)malloc(size);
        if (!nut) continue;
        if (!fseek(f, (long)offset, SEEK_SET) && fread(nut, 1, size, f) == size)
            loaded += menu_decode_nut(nut, size, &g_menu_art[id]);
        size_t bytes=(size_t)g_menu_art[id].width*g_menu_art[id].height*4;
        if (id >= 550 && id <= 617 && g_menu_art[id].pixels) {
            /* These authored 32x480 bevel strips carry transparent alignment
             * padding. Trim it once so their outside border meets our bounds. */
            menu_art* a=&g_menu_art[id];
            if(a->width==32 && a->height==480) {
                unsigned left=(id==550 || id==556 || id==561 || id==566 ||
                    id==571 || id==576 || id==581 || id==586 || id==591 ||
                    id==596 || id==616);
                unsigned x=left?6:0, w=left?26:25;
                for(unsigned y=0;y<461;++y)
                    memmove(a->pixels+y*w,a->pixels+(y+9)*32+x,w*4);
                a->width=w;a->height=461;
            }
        }
        if (id >= 641 && id <= 676 && g_menu_art[id].pixels) {
            menu_art* a=&g_menu_art[id];
            if (a->width==88 && a->height==24) {
                for (unsigned y=0;y<20;++y)
                    memmove(a->pixels+y*80,a->pixels+(y+2)*88+3,80*4);
                a->width=80;a->height=20;
            }
        }
        if (retained+bytes > 16u*1024u*1024u) {
            free(g_menu_art[id].pixels);memset(&g_menu_art[id],0,sizeof g_menu_art[id]);
            --loaded;
        } else retained+=bytes;
        free(nut);
    }
    fclose(f);
    fprintf(stderr, "[taiko_skin] loaded %u Green Song Select textures\n", loaded);
}
#endif
