#include "taiko_title_render.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

typedef struct SpineJob {
    const char* title;
    uint32_t reference[56*400];
    int native, failed;
} SpineJob;

static void* render_spines(void* data)
{
    SpineJob* job=data;
    uint32_t pixels[56*400];
    for(unsigned i=0;i<24;++i) {
        memset(pixels,0,sizeof pixels);
        int ok=job->native
            ? title_tex_render(TITLE_TEX_SONGLIST_SHORT,job->title,pixels,56,400,0x445566)
            : taiko_title_render_spine_argb(job->title,pixels,0);
        if(!ok || memcmp(pixels,job->reference,sizeof pixels)) job->failed=1;
    }
    return NULL;
}

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
    const char* spines[]={"J-POP","VOCALOID","CHILDREN'S SONGS",
        "ゲームミュージック！？", "ひゃく（テスト）ー", "A very long category title with punctuation!?"};
    for(unsigned t=0;t<sizeof spines/sizeof spines[0];++t) {
        memset(pixels,0,sizeof pixels);
        CHECK(taiko_title_render_spine_argb(spines[t],pixels,0));
        unsigned top=400, fill=0, outline=0;
        for(unsigned y=0;y<400;++y) for(unsigned x=0;x<56;++x) {
            uint32_t c=pixels[y*56+x];
            if(c>>24) {
                if(y<top) top=y;
                fill+=(c&0xffffff)==0xffffff;
                outline+=(c&0xffffff)==0;
            }
        }
        CHECK(top<=2 && fill>20 && outline>20);
    }
    // Full-width Latin S/T must remain upright in vertical song titles.
    // S is taller than wide; T's crossbar belongs above its narrow stem.
    const char* upright[]={"Ｓ","Ｔ"};
    for(unsigned letter=0;letter<2;++letter) {
        memset(pixels,0,sizeof pixels);
        CHECK(taiko_title_render_spine_scaled_argb(upright[letter],pixels,0,1));
        unsigned left=56,right=0,top=400,bottom=0;
        for(unsigned y=0;y<400;++y)for(unsigned x=0;x<56;++x)
            if(pixels[y*56+x]==0xffffffff) {
                if(x<left)left=x;if(x>right)right=x;
                if(y<top)top=y;if(y>bottom)bottom=y;
            }
        CHECK(left<=right && top<bottom);
        if(!letter) CHECK(bottom-top>right-left);
        else {
            unsigned upper=0,lower=0,third=(bottom-top+1)/3;
            for(unsigned y=top;y<=bottom;++y)for(unsigned x=left;x<=right;++x)
                if(pixels[y*56+x]==0xffffffff) {
                    if(y<top+third)++upper;
                    if(y>bottom-third)++lower;
                }
            CHECK(upper>lower*3/2);
        }
    }
    // Rebuilding at larger drawable sizes must retain real glyph coverage,
    // and changing scale back must not retain the previous profile's metrics.
    uint32_t *large=calloc(56*400*16,sizeof(uint32_t));
    CHECK(large);
    const unsigned scales[]={1,2,4,2,1};
    for(unsigned i=0;i<5;++i) {
        unsigned scale=scales[i], w=56*scale,h=400*scale,fill=0;
        CHECK(taiko_title_render_spine_scaled_argb("Anime",large,0,scale));
        for(unsigned j=0;j<w*h;++j) fill+=large[j]==0xffffffff;
        CHECK(fill>100*scale*scale);
        if(scale==1) {
            CHECK(taiko_title_render_spine_argb("Anime",pixels,0));
            CHECK(large[0]==0); // Host outline differs from the native vector stroke.
        }
    }
    free(large);
    // Browser and custom-song workers share the same mutable short-title
    // profile. Their different outline colours must not bleed into each other.
    SpineJob jobs[2]={{.title="CUSTOM TJA"},{.title="ゲーム！？",.native=1}};
    CHECK(taiko_title_render_spine_argb(jobs[0].title,jobs[0].reference,0));
    CHECK(title_tex_render(TITLE_TEX_SONGLIST_SHORT,jobs[1].title,
                          jobs[1].reference,56,400,0x445566));
    pthread_t threads[2];
    for(unsigned i=0;i<2;++i) CHECK(!pthread_create(&threads[i],NULL,render_spines,&jobs[i]));
    for(unsigned i=0;i<2;++i) { CHECK(!pthread_join(threads[i],NULL));CHECK(!jobs[i].failed); }
    return 0;
}
