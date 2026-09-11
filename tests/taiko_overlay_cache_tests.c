/* Test the real rasterizer/cache without exposing cache controls in the game. */
#define TAIKO_BROWSER_PREVIEW 1
double taiko_preview_clock_ms = -1;
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

static int settled_ui(float scale, HostUiInfo *info) {
    double deadline=text_profile_ms()+5000;
    for(;;) {
        native_count=native_texts=native_height=0;
        int ok=visit_host_ui(scale,collect_ui,NULL,info);
        int pending=0;
        pthread_mutex_lock(&g_async_lock);
        for(int i=0;i<ASYNC_TEXT_SLOTS;++i) {
            AsyncText *job=&g_async_text[i];
            if(job->state==1 || job->state==2 || (job->state==3 && text_profile_ms()-job->ready<100))pending=1;
        }
        pthread_mutex_unlock(&g_async_lock);
        if(!pending)return ok;
        assert(text_profile_ms()<deadline);
        struct timespec delay={0,1000000};nanosleep(&delay,NULL);
    }
}

static void check_menu_archive_bounds(void)
{
    unsigned char header[6500]={0};
    size_t table=0,base=0;uint32_t count=0;
    memcpy(header,"LM_NUT_TYPE1",12);
    // Empty skip block, one LM with a one-byte name, 788 NUT records.
    header[43]=1;header[56]=1;header[57]='x';
    size_t count_pos=58+5+16+5;
    header[count_pos+2]=3;header[count_pos+3]=20;
    assert(menu_archive_table(header,sizeof header,&table,&count,&base));
    assert(count==788 && table==97 && base==6417);
    for(size_t n=0;n<6417;++n)
        assert(!menu_archive_table(header,n,&table,&count,&base));
    header[16]=255; // Oversized skip section must never index outside the header.
    assert(!menu_archive_table(header,sizeof header,&table,&count,&base));
    unsigned char nut[112]={0};menu_art out={0};
    memcpy(nut,"NTP3",4);nut[7]=1;nut[27]=16;nut[35]=2;
    nut[37]=4;nut[39]=4;nut[96]=255; // Valid single 4x4 BC3 block.
    assert(menu_decode_nut(nut,sizeof nut,&out));free(out.pixels);
    for(size_t n=0;n<112;++n) assert(!menu_decode_nut(nut,n,&out));
    nut[36]=255;assert(!menu_decode_nut(nut,sizeof nut,&out));
}

int main(void)
{
    check_menu_archive_bounds();
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
    assert(settled_ui(1, &info));
    assert(native_count && native_texts);
    // Dynamic text IDs must never alias fixed menu artwork (0x40..0x48).
    pthread_mutex_lock(&g_async_lock);
    for(int i=0;i<ASYNC_TEXT_SLOTS;++i)if(g_async_text[i].state)
        assert((g_async_text[i].result.texture_id>>56)==0x49);
    pthread_mutex_unlock(&g_async_lock);
    unsigned first_count = native_count, first_height = native_height;
    uint64_t first_ids[512]; memcpy(first_ids, native_ids, sizeof first_ids);
    native_count = native_texts = native_height = 0;
    assert(settled_ui(1, &info));
    assert(native_count == first_count && !memcmp(first_ids, native_ids, first_count*sizeof(uint64_t)));
    native_count = native_texts = native_height = 0;
    assert(settled_ui(3, &info));
    assert(native_count == first_count && native_height > first_height * 2);
    taiko_overlay_song_row row = {0};
    row.title = "One song"; row.genre = "J-POP"; row.selected = 1;
    taiko_overlay_show_song_browser("P1", "fixture", "One song", "J-POP", 1,
        0, 1, 1, "J-POP", 0, 9, "ONI", 8, "", 0, 1, 0, &row, 1);
    assert(visit_host_ui(1, NULL, NULL, &info) && info.animated);
    g_song_animation_start = monotonic_milliseconds() - 600;
    // Even if the first poll is after the deadline, present the final position.
    assert(visit_host_ui(1, NULL, NULL, &info) && info.animated);
    native_count = 0;
    assert(settled_ui(1, &info));
    assert(visit_host_ui(1, NULL, NULL, &info) && !info.animated);
    // Folder-only levels have zero direct songs but must draw their rows.
    row.title = "Folder A"; row.kind = TAIKO_OVERLAY_ROW_CATEGORY;
    taiko_overlay_show_song_browser("P1", "", "", "CUSTOM TJA", 0,
        0, 0, 1, "CUSTOM TJA", 9, 10, "", 0, "", 0, 1, 1, &row, 1);
    g_song_animation_start = monotonic_milliseconds() - 600;
    native_count = 0;
    assert(settled_ui(1, &info));
    first_count = native_count;
    memcpy(first_ids, native_ids, first_count * sizeof(uint64_t));
    row.title = "Folder B";
    taiko_overlay_show_song_browser("P1", "", "", "CUSTOM TJA", 0,
        0, 0, 1, "CUSTOM TJA", 9, 10, "", 0, "", 0, 1, 1, &row, 1);
    g_song_animation_start = monotonic_milliseconds() - 600;
    native_count = 0;
    assert(settled_ui(1, &info));
    assert(native_count != first_count ||
           memcmp(first_ids, native_ids, first_count * sizeof(uint64_t)));
    // Incoming panels are opaque as a screen, with a black backing. Polling
    // alone must not spend the animation while the guest is busy loading.
    taiko_overlay_animate_browser(0);
    assert(visit_host_ui(1, NULL, NULL, &info) && info.animated && !info.overlay);
    assert(!g_handoff_snapshot);
    native_count = 0;
    assert(settled_ui(1, &info));
    assert(native_count == 4 && native_draws[0].colour == 0xff000000u);
    assert(native_draws[1].x == -96 && native_draws[2].x == 666);
    uint64_t panel_id = native_draws[1].texture_id;
    g_handoff_start = monotonic_milliseconds() - HANDOFF_MS / 2;
    native_count = 0;
    assert(settled_ui(1, &info));
    assert(native_draws[1].texture_id == panel_id); // No animated texture churn.
    assert(native_draws[1].x >= -49 && native_draws[1].x <= -47);
    assert((native_draws[1].colour >> 24) >= 127 && (native_draws[1].colour >> 24) <= 129);
    g_handoff_start = monotonic_milliseconds() - HANDOFF_MS - 1;
    assert(visit_host_ui(1, NULL, NULL, &info) && !info.animated && !info.overlay);

    taiko_overlay_animate_browser(1);
    assert(visit_host_ui(1, NULL, NULL, &info) && info.animated && info.overlay);
    native_count = 0;
    assert(settled_ui(1, &info));
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
    // GPU-only portraits must survive the CPU panel snapshot used at handoff.
    // Unjoined slots emit nothing; P2 alone is mirrored and stays on its panel.
    g_visible = 1; g_mode = 5;
    taiko_overlay_set_browser_portrait(0, 0xc1000000, 600, 600);
    taiko_overlay_set_browser_portrait(1, 0xc1200000, 600, 600);
    taiko_overlay_set_browser_players(1, 0, 0, NULL);
    native_count = 0;
    assert(settled_ui(1, &info));
    for (unsigned i = 0; i < native_count; ++i) assert(!native_draws[i].surface_address);
    taiko_overlay_set_browser_players(1, 2, 0, NULL);
    assert(taiko_overlay_browser_joined() == 2);
    native_count = 0;
    assert(settled_ui(1, &info) && info.animated);
    unsigned portraits = 0;
    for (unsigned i = 0; i < native_count; ++i) {
        if (!native_draws[i].surface_address) continue;
        assert(native_draws[i].surface_address == 0xc1200000 && native_draws[i].flip_x);
        ++portraits;
    }
    assert(portraits == 1);
    taiko_overlay_set_browser_players(1, 3, 0, NULL);
    taiko_overlay_animate_browser(1);
    native_count = 0;
    assert(settled_ui(1, &info));
    assert(native_count == 5); // Three panels plus two native surfaces.
    assert(native_draws[1].surface_address == 0xc1000000 && !native_draws[1].flip_x);
    assert(native_draws[2].surface_address == 0xc1200000 && native_draws[2].flip_x);
    g_handoff_start = monotonic_milliseconds() - HANDOFF_MS / 2;
    native_count = 0;
    assert(settled_ui(1, &info));
    assert(native_draws[1].x == 178 + native_draws[0].x);
    assert(native_draws[1].colour == native_draws[0].colour);
    assert(native_draws[2].colour == native_draws[0].colour);
    assert(taiko_overlay_browser_visible());
    g_handoff_start = monotonic_milliseconds() - HANDOFF_MS - 1;
    assert(!taiko_overlay_browser_visible());
    taiko_overlay_show_entry_menu(0);
    taiko_overlay_animate_browser(1); // Never animate login/pairing accidentally.
    assert(visit_host_ui(1, NULL, NULL, &info) && !info.overlay);
    taiko_overlay_hide_host_screen();
    assert(!visit_host_ui(3, NULL, NULL, &info));
    // Category portraits occupy opposite bottom corners, including handoff.
    row.title="J-POP";row.kind=TAIKO_OVERLAY_ROW_CATEGORY;row.selected=1;
    taiko_overlay_show_song_browser("P1 + P2","","J-POP","",0,
        0,12,1,"CATEGORIES",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_CATEGORIES,0,&row,1);
    native_count=0;
    assert(visit_host_ui(1,collect_ui,NULL,&info));
    portraits=0;
    for(unsigned i=0;i<native_count;++i) if(native_draws[i].surface_address) {
        assert(native_draws[i].y==190);
        assert(native_draws[i].w==620 && native_draws[i].h==620);
        assert(!native_draws[i].flip_x);
        assert(native_draws[i].x==(portraits?845:-185));
        ++portraits;
    }
    assert(portraits==2);
    taiko_overlay_animate_browser(1);
    native_count=0;assert(visit_host_ui(1,collect_ui,NULL,&info));
    g_handoff_start=monotonic_milliseconds()-HANDOFF_MS/2;
    native_count=0;assert(visit_host_ui(1,collect_ui,NULL,&info));
    for(unsigned i=0;i<native_count;++i) if(native_draws[i].surface_address)
        assert(native_draws[i].x>845 || native_draws[i].x< -185);
    // A centred category window must stay ordered through steps and wraparound.
    const char* carousel_titles[]={"C0","C1","C2","C3","C4","C5","C6","C7","C8","C9","C10","C11"};
    const unsigned selections[]={0,1,0,11,0};
    taiko_overlay_song_row carousel[TAIKO_OVERLAY_SONG_ROW_COUNT]={0};
    for(unsigned step=0;step<5;++step) {
        g_song_animation_start=monotonic_milliseconds()-1000;
        for(unsigned r=0;r<TAIKO_OVERLAY_SONG_ROW_COUNT;++r) {
            unsigned category=(selections[step]+12-5+r)%12;
            carousel[r].title=carousel_titles[category];
            carousel[r].catalog_index=category;
            carousel[r].kind=TAIKO_OVERLAY_ROW_CATEGORY;
            carousel[r].selected=r==5;
        }
        taiko_overlay_show_song_browser("P1 + P2","",carousel_titles[selections[step]],"",0,
            0,12,1,"CATEGORIES",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_CATEGORIES,0,
            carousel,TAIKO_OVERLAY_SONG_ROW_COUNT);
        // During the idle delay, the selected spine stays closed and centred.
        g_song_animation_start=monotonic_milliseconds()-200;
        float hold_x,hold_w;
        menu_card_pose(g_song_rows[5].from_card_x,g_song_rows[5].from_card_w,0,
                       song_ease(),&hold_x,&hold_w);
        assert(fabsf(hold_w-76)<0.01f && fabsf(hold_x+hold_w/2-640)<0.01f);
        assert(menu_card_x(-5)<0 && menu_card_x(-5)+menu_card_w(-5)>0);
        assert(menu_card_x(5)<1280 && menu_card_x(5)+menu_card_w(5)>1280);
        if(!step) continue;
        for(unsigned tick=0;tick<=4;++tick) {
            float t=tick/4.0f;
            for(unsigned r=0;r+1<TAIKO_OVERLAY_SONG_ROW_COUNT;++r) {
                float x,w,next,next_w;
                menu_card_pose(g_song_rows[r].from_card_x,g_song_rows[r].from_card_w,(int)r-5,t,&x,&w);
                menu_card_pose(g_song_rows[r+1].from_card_x,g_song_rows[r+1].from_card_w,(int)r-4,t,&next,&next_w);
                if(t>=0.45f) {
                    if(r==5) assert(fabsf(x+w/2-640)<0.01f);
                    else assert(fabsf(x-menu_card_x((int)r-5))<0.01f);
                }
                assert(x+w<=next); // No newly revealed card underneath its neighbour.
            }
        }
    }
    // Rapid navigation skips directly to the next closed centre spine.
    g_song_animation_start=monotonic_milliseconds()-20;
    for(unsigned r=0;r<TAIKO_OVERLAY_SONG_ROW_COUNT;++r) {
        unsigned category=(1+12-5+r)%12;
        carousel[r].title=carousel_titles[category];
        carousel[r].catalog_index=category;
    }
    taiko_overlay_show_song_browser("P1 + P2","","C1","",0,
        0,12,1,"CATEGORIES",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_CATEGORIES,0,
        carousel,TAIKO_OVERLAY_SONG_ROW_COUNT);
    assert(song_ease()==0.45f);
    float rapid_x,rapid_w;
    menu_card_pose(g_song_rows[5].from_card_x,g_song_rows[5].from_card_w,0,
                   song_ease(),&rapid_x,&rapid_w);
    assert(rapid_x==602 && rapid_w==76);
    double rapid_start=g_song_animation_start;
    taiko_overlay_show_song_browser("P1 + P2","","C1","",0,
        0,12,1,"CATEGORIES",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_CATEGORIES,0,
        carousel,TAIKO_OVERLAY_SONG_ROW_COUNT);
    assert(g_song_animation_start==rapid_start);
    // Confirmation owns the outgoing rows; publishing the catalog again must
    // neither lose the left categories nor restart the opening animation.
    taiko_overlay_song_row opened[2]={
        {"Return","J-POP",0,1,TAIKO_OVERLAY_ROW_EXIT},
        {"Lemon","J-POP",1,0,TAIKO_OVERLAY_ROW_SONG}};
    taiko_overlay_show_song_browser("P1","","Return","J-POP",0,
        0,1,1,"J-POP",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_SONGS,1,opened,2);
    assert(g_folder_open && g_folder_category_count==TAIKO_OVERLAY_SONG_ROW_COUNT);
    double opening_start=g_folder_open_start;
    char retained_title[256];
    strcpy(retained_title,g_folder_categories[0].title);
    taiko_overlay_show_song_browser("P1","","Return","J-POP",0,
        0,1,1,"J-POP",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_SONGS,1,opened,2);
    assert(g_folder_open_start==opening_start);
    assert(!strcmp(retained_title,g_folder_categories[0].title));
    taiko_overlay_show_song_browser("P1","","C1","",0,
        0,12,1,"CATEGORIES",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_CATEGORIES,0,
        carousel,TAIKO_OVERLAY_SONG_ROW_COUNT);
    assert(!g_folder_open && !g_folder_category_count);
    // Search and nested custom-folder lists use their existing transition.
    opened[0].kind=TAIKO_OVERLAY_ROW_CATEGORY;
    taiko_overlay_show_song_browser("P1","","Nested","CUSTOM TJA",0,
        0,1,1,"CUSTOM TJA",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_SONGS,1,opened,2);
    assert(!g_folder_open);
    // Beginning, interior, and end share one close. Even a million-entry
    // library emits only a viewport-sized folder, with a fixed title tab.
    for(unsigned sample=0;sample<3;++sample) {
        const unsigned position=sample==0?0:sample==1?500000:1000000;
        taiko_overlay_show_song_browser("P1","","J-POP","",0,
            0,12,1,"CATEGORIES",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_CATEGORIES,0,
            carousel,TAIKO_OVERLAY_SONG_ROW_COUNT);
        opened[0].kind=TAIKO_OVERLAY_ROW_EXIT;
        opened[0].browser_position=position;opened[0].browser_total=1000001;
        opened[1].browser_position=position;opened[1].browser_total=1000001;
        taiko_overlay_show_song_browser("P1","","Return","J-POP",0,
            0,1,1,"J-POP",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_SONGS,1,opened,2);
        g_folder_scroll_start=monotonic_milliseconds()-200;
        g_folder_open_start=monotonic_milliseconds()-1000;
        g_song_animation_start=monotonic_milliseconds()-500;
        double scroll_start=g_folder_scroll_start;
        taiko_overlay_show_song_browser("P1","","Return","J-POP",0,
            0,1,1,"J-POP",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_SONGS,1,opened,2);
        assert(g_folder_scroll_start==scroll_start);
        native_count=0;assert(visit_host_ui(1,collect_ui,NULL,&info));
        unsigned shell_draws=0;
        for(unsigned i=0;i<native_count;++i)
            if((native_draws[i].texture_id>>56)==0x48) {
                ++shell_draws;
                assert(native_draws[i].w<=1472 && native_draws[i].x>=-96);
            }
        assert(shell_draws==5);
        taiko_overlay_show_song_browser("P1","","J-POP","",0,
            0,12,1,"CATEGORIES",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_CATEGORIES,0,
            carousel,TAIKO_OVERLAY_SONG_ROW_COUNT);
        assert(g_folder_closing && !g_folder_open && g_folder_close_count==2);
        double close_start=g_folder_close_start;
        taiko_overlay_show_song_browser("P1","","J-POP","",0,
            0,12,1,"CATEGORIES",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_CATEGORIES,0,
            carousel,TAIKO_OVERLAY_SONG_ROW_COUNT);
        assert(g_folder_close_start==close_start);
        g_folder_close_start=monotonic_milliseconds()-450;
        native_count=0;assert(visit_host_ui(1,collect_ui,NULL,&info));
        unsigned edges=0;
        for(unsigned i=0;i<native_count;++i) {
            if(native_draws[i].texture_id==UINT64_C(0x4800000000000000)) {
                assert(native_draws[i].x==430);++edges;
            }
            if(native_draws[i].texture_id==UINT64_C(0x4800000000000003)) {
                assert(native_draws[i].x==838);++edges;
            }
        }
        assert(edges==2); // Both arrived, independent of their travel distance.
        g_folder_close_start=monotonic_milliseconds()-900;
        native_count=0;assert(visit_host_ui(1,collect_ui,NULL,&info));
        assert(!g_folder_closing);
    }
    // Fading slices must not overlap: identical body pixels blend once.
    background(0xff000000);
    g_menu_alpha=128;
    menu_open_shell(&menu_styles[0],54,573,430,1330);
    assert(g_pixels[300*HOST_WIDTH+455]==g_pixels[300*HOST_WIDTH+640]);
    assert(g_pixels[300*HOST_WIDTH+640]==g_pixels[300*HOST_WIDTH+1100]);
    assert(g_pixels[300*HOST_WIDTH+640]!=0xff000000);
    g_menu_alpha=255;

    // At the final close pose the blue shell must be completely occluded,
    // not merely faded. Compare against normal categories at the SAME time.
    taiko_preview_clock_ms=50201;
    for(unsigned i=0;i<TAIKO_OVERLAY_SONG_ROW_COUNT;++i) {
        unsigned category=(12-5+i)%12;
        carousel[i].title=menu_styles[category].label;
        carousel[i].genre=carousel[i].title;
        carousel[i].selected=i==5;carousel[i].catalog_index=84;
    }
    taiko_overlay_show_song_browser("P1","","J-POP","",0,
        0,12,1,"CATEGORIES",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_CATEGORIES,0,
        carousel,TAIKO_OVERLAY_SONG_ROW_COUNT);
    taiko_preview_clock_ms+=1000;
    opened[0].browser_position=0;opened[0].browser_total=11;
    opened[1].browser_position=1;opened[1].browser_total=11;
    taiko_overlay_show_song_browser("P1","","Return","J-POP",0,
        0,1,1,"J-POP",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_SONGS,1,opened,2);
    taiko_preview_clock_ms+=1000;
    taiko_overlay_show_song_browser("P1","","J-POP","",0,
        0,12,1,"CATEGORIES",0,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_CATEGORIES,0,
        carousel,TAIKO_OVERLAY_SONG_ROW_COUNT);
    taiko_preview_clock_ms+=799;
    background(0xff000000);render_green_categories();
    memcpy(reference,g_pixels,sizeof reference);
    assert(g_folder_closing);
    g_folder_closing=0;
    background(0xff000000);render_green_categories();
    for(unsigned y=40;y<565;++y) for(unsigned x=400;x<880;++x) {
        if(x>=440 && x<840 && y>=132 && y<553) continue; // Contents still fading.
        if(reference[y*HOST_WIDTH+x]!=g_pixels[y*HOST_WIDTH+x]) {
            fprintf(stderr,"close uncovered at %u,%u: %08x != %08x\n",x,y,reference[y*HOST_WIDTH+x],g_pixels[y*HOST_WIDTH+x]);
            assert(0);
        }
    }
    taiko_preview_clock_ms=-1;
    for (unsigned i = 0; i < TEXT_CACHE_COUNT; ++i) release_text_bitmap(&g_text_cache[i]);
    assert(g_text_cache_bytes == 0);
    return 0;
}
