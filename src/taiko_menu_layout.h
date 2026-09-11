#include "taiko_title_render.h"

/* Category-screen composition in Green's logical 1280x720 coordinates. */
static int green_categories(void)
{
    if (g_mode != 5) return 0;
    if (g_song_browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES) return 1;
    if (g_song_search_active || g_song_query[0] || !g_song_row_count) return 0;
    if(g_song_shared_carousel)return 1;
    for (unsigned i=0;i<g_song_row_count;++i)
        if (g_song_rows[i].kind == TAIKO_OVERLAY_ROW_DIFFICULTY ||
            g_song_rows[i].kind == TAIKO_OVERLAY_ROW_CATEGORY) return 0;
    return 1;
}

static unsigned g_menu_alpha=255;

static int menu_bitmap(const menu_art* art, uint64_t texture_id,
                       float x, float y, float w, float h, int flip)
{
    if (!art->pixels || w <= 0 || h <= 0) return 0;
    if (g_ui_emit) {
        HostUiDraw d = {0};
        d.x=x; d.y=y; d.w=w; d.h=h; d.colour=(g_menu_alpha<<24)|0xffffff;
        d.texture_id=texture_id;
        d.pixels=art->pixels; d.width=art->width; d.height=art->height; d.flip_x=flip;
        g_ui_emit(g_ui_user,&d);
    } else {
        int x0 = x < 0 ? 0 : (int)x, y0 = y < 0 ? 0 : (int)y;
        int x1 = x+w > g_width ? g_width : (int)(x+w);
        int y1 = y+h > g_height ? g_height : (int)(y+h);
        for (int py=y0;py<y1;++py) for(int px=x0;px<x1;++px) {
            unsigned sx=(unsigned)((px-x)*art->width/w), sy=(unsigned)((py-y)*art->height/h);
            if(sx>=art->width || sy>=art->height) continue;
            if(flip) sx=art->width-1-sx;
            uint32_t c=art->pixels[sy*art->width+sx];
            put_pixel(px,py,c,g_menu_alpha);
        }
    }
    return 1;
}

static int menu_image(unsigned id, float x, float y, float w, float h, int flip)
{
    return id < 788 && menu_bitmap(&g_menu_art[id],
        UINT64_C(0x4000000000000000)+id, x,y,w,h,flip);
}

/* Cached soft silhouettes keep the CPU and GPU compositors' blending identical. */
static void menu_shadow(float x, float y, int w, int h, int radius, unsigned slot)
{
    static menu_art shadows[3];
    menu_art* art=&shadows[slot];
    const int pad=6;
    if (!art->pixels) {
        art->width=w+pad*2; art->height=h+pad*2;
        art->pixels=calloc(art->width*art->height,sizeof(uint32_t));
        if (!art->pixels) return;
        for(int py=0;py<(int)art->height;++py) for(int px=0;px<(int)art->width;++px) {
            unsigned coverage=0;
            for(int dy=-4;dy<=4;++dy) for(int dx=-4;dx<=4;++dx) {
                int sx=px-pad+dx, sy=py-pad+dy;
                if(sx<0 || sx>=w || sy<0 || sy>=h) continue;
                int cx=sx<radius?radius:sx>=w-radius?w-radius-1:sx;
                int cy=sy<radius?radius:sy>=h-radius?h-radius-1:sy;
                if((sx-cx)*(sx-cx)+(sy-cy)*(sy-cy)<=radius*radius) ++coverage;
            }
            art->pixels[py*art->width+px]=(coverage*65/81)<<24;
        }
    }
    menu_bitmap(art,UINT64_C(0x4600000000000000)+slot,
                x+5-pad,y+5-pad,w+pad*2,h+pad*2,0);
}

typedef struct menu_folder_style { const char* label; uint32_t colour; unsigned tab, edge; } menu_folder_style;
static const menu_folder_style menu_styles[] = {
    {"J-POP",RGB_COLOUR(32,158,183),643,557},
    {"ANIME",RGB_COLOUR(255,153,0),645,562},
    {"VOCALOID",RGB_COLOUR(201,210,230),667,617},
    {"VARIETY",RGB_COLOUR(143,208,10),649,572},
    {"CLASSICAL",RGB_COLOUR(205,163,13),651,577},
    {"GAME MUSIC",RGB_COLOUR(156,119,183),653,582},
    {"NAMCO ORIGINAL",RGB_COLOUR(255,88,9),655,587},
    {"MEDLEY",RGB_COLOUR(205,180,49),659,597},
    {"CHILDREN'S SONGS",RGB_COLOUR(252,65,137),647,567},
    {"CUSTOM TJA",RGB_COLOUR(50,194,58),641,551},
    {"OSU! LAZER",RGB_COLOUR(252,65,137),647,567},
    {"NIJIIRO",RGB_COLOUR(255,107,97),657,592},
};
static const menu_folder_style* menu_style(const char* title)
{
    for(unsigned i=0;i<sizeof menu_styles/sizeof menu_styles[0];++i)
        if(!strcmp(title,menu_styles[i].label)) return &menu_styles[i];
    return &menu_styles[9];
}
static uint32_t menu_outline(const menu_folder_style* style)
{
    uint32_t c=style->colour;
    unsigned r=(c&255)*3/5,g=((c>>8)&255)*3/5,b=((c>>16)&255)*3/5;
    return (r<<16)|(g<<8)|b;
}

static unsigned menu_style_index(const menu_folder_style* style)
{
    return (unsigned)(style-menu_styles);
}
static unsigned menu_background(const menu_folder_style* style)
{
    static const unsigned ids[]={772,773,783,775,776,777,778,780,774,771,774,779};
    return ids[menu_style_index(style)];
}
static unsigned menu_character(const menu_folder_style* style)
{
    static const unsigned ids[]={507,509,523,513,515,517,519,525,511,505,521,525};
    return ids[menu_style_index(style)];
}

static float menu_card_x(int relative)
{
    return relative < 0 ? 440 + relative*96 : relative > 0 ? 764 + relative*96 : 440;
}
static float menu_card_w(int relative) { return relative ? 76 : 400; }

/* Keep the full centre slot reserved. Neighbours finish sliding before
 * the selected spine opens; expansion must never push the row outward. */
static void menu_card_pose(float from_x,float from_w,int relative,float progress,
                           float* x,float* w)
{
    const float closed_x=relative?menu_card_x(relative):602;
    if(progress<0.45f) {
        float t=progress/0.45f;
        float closed_from=from_x+(from_w-76)/2;
        *x=closed_from+(closed_x-closed_from)*t;
        *w=76;
    } else {
        float t=(progress-0.45f)/0.55f;
        *w=76+(menu_card_w(relative)-76)*t;
        *x=closed_x+(menu_card_x(relative)-closed_x)*t;
    }
}

static menu_art g_menu_middle[788];

static void menu_folder(float x, float y, float w, float h,
                         const menu_folder_style* style, int tab)
{
    menu_shadow(x,y,(int)w,(int)h,0,w>100?1:0);
    const menu_art* left = &g_menu_art[style->edge-1];
    const menu_art* right = &g_menu_art[style->edge];
    menu_art* middle = &g_menu_middle[style->edge];
    /* The strip's inside column includes the original top/bottom bevel and
     * body gradient. Repeating it joins both corners without guessed colours. */
    if (!middle->pixels && left->pixels && right->pixels &&
        left->height == right->height) {
        middle->pixels = malloc(left->height * sizeof(uint32_t));
        if (middle->pixels) {
            middle->width = 1; middle->height = left->height;
            for (unsigned row=0; row<left->height; ++row)
                middle->pixels[row] = left->pixels[row*left->width+left->width-1];
        }
    }
    const float tab_scale=h/461.0f;
    float tab_width = w < 80*tab_scale ? w : 80*tab_scale;
    if (!middle->pixels) {
        fill_rect(x,y,x+w,y+h,0xff000000);
        fill_rect(x+6,y+6,x+w-6,y+h-6,style->colour);
        if(tab) fill_rounded_rect(x,y-14,x+tab_width*0.6f,y+5,7,style->colour);
        return;
    }
    float lw = left->width*h/left->height, rw = right->width*h/right->height;
    menu_bitmap(middle,UINT64_C(0x4100000000000000)+style->edge,
                x+lw,y,w-lw-rw,h,0);
    menu_image(style->edge-1,x,y,lw,h,0);
    menu_image(style->edge,x+w-rw,y,rw,h,0);
    /* The authored tab includes the top border outside its raised section.
     * Put it over the frame so the frame cannot draw a line through the tab. */
    if (tab && !menu_image(style->tab,x+1,y-g_menu_tab_baseline[style->tab]*tab_scale,tab_width,20*tab_scale,0))
        fill_rounded_rect(x,y-14,x+tab_width*0.6f,y+5,7,0xff000000);
 }

/* Selected cards use Green's yellow panel; extend its flat middle while
 * retaining the authored bevel corner sizes. */
static menu_art g_menu_yellow[3];
static void menu_yellow_frame(float x,float y,float w,float h)
{
    const menu_art* source=&g_menu_art[354];
    if(source->pixels && source->width>=32 && source->height>=32 && !g_menu_yellow[0].pixels) {
        for(unsigned part=0;part<3;++part) {
            menu_art* a=&g_menu_yellow[part];
            a->width=part==1?1:16;a->height=461;
            a->pixels=calloc(a->width*a->height,4);
            if(!a->pixels)continue;
            for(unsigned yy=0;yy<a->height;++yy) {
                unsigned sy=yy<16?yy:yy>=445?source->height-(461-yy):source->height/2;
                for(unsigned xx=0;xx<a->width;++xx) {
                    unsigned sx=part==0?xx:part==1?source->width/2:source->width-16+xx;
                    a->pixels[yy*a->width+xx]=source->pixels[sy*source->width+sx];
                }
            }
        }
    }
    if(!g_menu_yellow[0].pixels || !g_menu_yellow[1].pixels || !g_menu_yellow[2].pixels) {fill_rect(x+7,y+7,x+w-7,y+h-7,RGB_COLOUR(255,225,49));return;}
    menu_bitmap(&g_menu_yellow[0],UINT64_C(0x4400000000000000),x,y,16,h,0);
    menu_bitmap(&g_menu_yellow[1],UINT64_C(0x4400000000000001),x+16,y,w-32,h,0);
    menu_bitmap(&g_menu_yellow[2],UINT64_C(0x4400000000000002),x+w-16,y,16,h,0);
}

/* Native short-title textures keep Zucchini's spacing, UTF-8 punctuation and
 * top anchoring. Each immutable category label becomes one cached GPU quad. */
static struct {
    char title[256];
    menu_art art;
    uint32_t outline;
    uint64_t id;
} g_menu_spines[16];
static uint64_t g_menu_spine_generation;
static unsigned g_menu_spine_next;

static void menu_spine(const char* text, float x, float y, float height, uint32_t outline)
{
    unsigned scale=(unsigned)ceilf(height / 400 * (g_ui_emit ? g_ui_scale : 1));
    if(scale<1)scale=1; if(scale>4)scale=4;
    if(g_ui_emit) {
        text_cache_entry key={0};
        snprintf(key.text,sizeof key.text,"%s",text);
        float fade=1;
        text_cache_entry *ready=async_text(&key,1,scale,outline,&fade);
        if(!ready)return;
        menu_art art={0};art.width=ready->width;art.height=ready->height;art.pixels=ready->bitmap;
        unsigned alpha=g_menu_alpha;g_menu_alpha=(unsigned)(alpha*fade);
        float width=height*56/400;
        menu_bitmap(&art,ready->texture_id,x-width/2,y,width,height,0);
        g_menu_alpha=alpha;return;
    }
    unsigned slot;
    for (slot=0; slot<16; ++slot)
        if (g_menu_spines[slot].art.pixels && !strcmp(text,g_menu_spines[slot].title) && g_menu_spines[slot].outline==outline && g_menu_spines[slot].art.width==56*(int)scale) break;
    if (slot==16) {
        double profile_start=text_profile_ms();
        slot=g_menu_spine_next++%16;
        menu_art* art=&g_menu_spines[slot].art;
        free(art->pixels);
        art->width=56*scale; art->height=400*scale;
        art->pixels=calloc(art->width*art->height,sizeof(uint32_t));
        if (!art->pixels) return;
        if (!taiko_title_render_spine_scaled_argb(text,art->pixels,outline,scale)) {
            free(art->pixels);art->pixels=NULL;return;
        }
        /* The guest title renderer returns ARGB words; host UI uses RGBA. */
        for (unsigned i=0;i<(unsigned)(art->width*art->height);++i) {
            uint32_t c=art->pixels[i];
            art->pixels[i]=(c&0xff00ff00u)|((c>>16)&255)|((c&255)<<16);
        }
        if(getenv("TAIKO_TEXT_PROFILE")) fprintf(stderr,"[TEXT] spine scale=%u ms=%.3f text=%s\n",scale,text_profile_ms()-profile_start,text);
        snprintf(g_menu_spines[slot].title,sizeof g_menu_spines[slot].title,"%s",text);
        g_menu_spines[slot].outline=outline;
        g_menu_spines[slot].id=UINT64_C(0x4200000000000000)+ ++g_menu_spine_generation;
    }
    float width=height*56/400;
    menu_bitmap(&g_menu_spines[slot].art,g_menu_spines[slot].id,
                x-width/2,y,width,height,0);
}

static void menu_title(const char* title,float x,float y,float width)
{
    if(text_width(title,36)<=width) { draw_text_at(title,36,x,y);return; }
    const char* split=strchr(title,' ');
    if(!split) { draw_text_fit(title,36,width,x,y);return; }
    char first[128]; size_t n=(size_t)(split-title);
    if(n>=sizeof first) n=sizeof first-1;
    memcpy(first,title,n);first[n]=0;
    draw_text_fit(first,34,width,x,y-22);
    draw_text_fit(split+1,34,width,x,y+22);
}

/* One second of smooth fade-in/out followed by one second at rest. */
static float menu_prompt_phase(void)
{
    double now=monotonic_milliseconds();
    return (float)((now-(uint64_t)(now/2000.0)*2000.0)/1000.0);
}

static unsigned menu_prompt_alpha(float phase)
{
    if(phase>=1) return 0;
    float bell=phase*(1-phase);
    return (unsigned)(255*16*bell*bell+0.5f);
}

static void menu_control_drums(void)
{
    const float y=577, size=50, unit=size/56;
    float phase=menu_prompt_phase();
    unsigned saved_alpha=g_menu_alpha;
    menu_bitmap(&g_menu_indicator[2],UINT64_C(0x4300000000000002),430,y,size,size,0);
    menu_bitmap(&g_menu_indicator[2],UINT64_C(0x4300000000000002),650,y,size,size,0);
    g_menu_alpha=menu_prompt_alpha(phase);
    if(g_menu_alpha) {
        float travel=8*phase;
        menu_bitmap(&g_menu_indicator[3],UINT64_C(0x4300000000000003),650,y,size,size,0);
        /* Each 24px half ends at the drum's authored centre (28,28).
         * The source's transparent padding is part of that alignment. */
        menu_bitmap(&g_menu_indicator[5],UINT64_C(0x4300000000000005),430+4*unit,y,24*unit,size,0);
        menu_bitmap(&g_menu_indicator[5],UINT64_C(0x4300000000000005),430+28*unit,y,24*unit,size,1);
        menu_bitmap(&g_menu_indicator[6],UINT64_C(0x4300000000000006),415-travel,y+15,20,20,0);
        menu_bitmap(&g_menu_indicator[6],UINT64_C(0x4300000000000006),475+travel,y+15,20,20,1);
    }
    g_menu_alpha=saved_alpha;
}

static void menu_navigation_arrows(void)
{
    double now=monotonic_milliseconds();
    float phase=(float)((now-(uint64_t)(now/1000.0)*1000.0)/1000.0);
    unsigned saved_alpha=g_menu_alpha;
    g_menu_alpha=menu_prompt_alpha(phase);
    if(g_menu_alpha) {
        float travel=18*phase, w=84, h=w*120/104;
        menu_image(787,448-w-travel,349-h/2,w,h,0);
        menu_image(787,832+travel,349-h/2,w,h,1);
    }
    g_menu_alpha=saved_alpha;
}

static menu_art g_menu_nameplates[2];
static void menu_nameplate(unsigned player,float left,const char* name)
{
    menu_art* plate=&g_menu_nameplates[player];
    const menu_art* source=&g_menu_indicator[214];
    if(!plate->pixels && source->pixels) {
        plate->width=source->width;plate->height=source->height;
        plate->pixels=malloc((size_t)plate->width*plate->height*4);
        if(plate->pixels) for(unsigned i=0;i<plate->width*plate->height;++i) {
            uint32_t c=source->pixels[i];
            unsigned r=c&255,g=(c>>8)&255,b=(c>>16)&255;
            plate->pixels[i]=(c&0xff000000u)|
                (r*(player?206:255)/255)|
                ((g*(player?241:198)/255)<<8)|
                ((b*(player?244:190)/255)<<16);
        }
    }
    const float top=632;
    if(plate->pixels) {
        menu_bitmap(&g_menu_indicator[212],UINT64_C(0x43000000000000d4),left+20,top+2,268,64,0);
        menu_bitmap(&g_menu_indicator[player?234:213],UINT64_C(0x4300000000000000)+(player?234:213),left+20,top,268,32,0);
        menu_bitmap(plate,UINT64_C(0x4500000000000000)+player,left+20,top,268,64,0);
        menu_bitmap(&g_menu_indicator[player?235:215],UINT64_C(0x4300000000000000)+(player?235:215),left,top,64,64,0);
    } else {
        fill_rounded_rect(left,top,left+288,top+64,32,0xff000000);
        fill_rounded_rect(left+4,top+4,left+284,top+60,28,player?RGB_COLOUR(104,191,192):RGB_COLOUR(255,71,40));
        draw_text_at(player?"2P":"1P",25,left+32,top+32);
    }
    int saved_outline=g_menu_text_outline;
    g_menu_text_outline=2;
    draw_text_fit(name,20,210,left+154,top+45);
    g_menu_text_outline=saved_outline;
    g_outline_radius=saved_outline;
}

/* The expanded title is a shallow, open-bottom trapezoid. Do not enlarge
 * the small panel sprite: its baked border becomes thick and irregular.
 * Rasterize at 3x with coverage AA, shared by CPU and GPU composition. */
static int menu_tab_contains(float x, float y, int inner)
{
    const float inset=inner?6.0f:0.0f;
    const float radius=10-inset;
    if(y<inset) return 0;
    if(x>172) x=344-x;
    /* Tangent join between the rounded shoulder and the slanted side.
     * Offset the side along its normal so its stroke matches the top. */
    if(y<10-radius*0.177153f) {
        if(x>=22) return 1;
        float dx=x-22,dy=y-10;
        return dx*dx+dy*dy<=radius*radius;
    }
    return x>=13.63929f-y*0.18f+inset*1.016071f;
}

static void menu_heading(const menu_folder_style* style, const char* title, float opening, int text)
{
    static menu_art tabs[12];
    unsigned slot=(unsigned)(style-menu_styles);
    menu_art* tab=&tabs[slot];
    if(!tab->pixels) {
        tab->width=344*3;tab->height=64*3;
        tab->pixels=calloc((size_t)tab->width*tab->height,4);
        if(tab->pixels) for(unsigned y=0;y<tab->height;++y) for(unsigned x=0;x<tab->width;++x) {
            unsigned cover=0,fill=0;
            for(unsigned sy=0;sy<2;++sy) for(unsigned sx=0;sx<2;++sx) {
                float px=(x+(sx+0.5f)/2)/3,py=(y+(sy+0.5f)/2)/3;
                cover+=menu_tab_contains(px,py,0);
                fill+=menu_tab_contains(px,py,1);
            }
            if(cover) tab->pixels[y*tab->width+x]=((cover*255/4)<<24)|
                ((style->colour&255)*fill/cover)|
                ((((style->colour>>8)&255)*fill/cover)<<8)|
                ((((style->colour>>16)&255)*fill/cover)<<16);
        }
    }
    float rise=(opening-0.13f)/0.4f;
    if(rise<0) rise=0;
    if(rise>1) rise=1;
    menu_bitmap(tab,UINT64_C(0x4700000000000000)+slot,468,138-64*rise,344,64*rise,0);
    if (!text) return;
    int saved_outline=g_menu_text_outline;
    g_menu_text_outline=5;
    g_text_opacity=(unsigned)(255*rise);
    draw_text_fit(title,38,300,640,138-32*rise);
    g_text_opacity=255;
    g_menu_text_outline=saved_outline;
    g_outline_radius=saved_outline;
}

static void menu_song_courses(const song_row_storage* row, const menu_folder_style* active,
                              float x, float w)
{
    static const char* courses[]={"EASY","NORMAL","HARD","ONI","URA"};
    unsigned shown=0;
    for(unsigned d=0;d<5;++d) if(g_song_difficulty_mask&(1u<<d)) ++shown;
    unsigned column=0;
    for(unsigned d=0;d<5;++d) if(g_song_difficulty_mask&(1u<<d)) {
        float cx=x+36+column++*(w-144)/(shown?shown:1);
        fill_rounded_rect(cx,279,cx+38,511,18,active->colour);
        static const unsigned icons[]={259,260,261,262,326};
        menu_image(icons[d],cx-3,247,44,35,0);
        menu_spine(courses[d],cx+19,296,180,0);
        unsigned rating=row->course_stars[d];
        if(!rating) draw_text_at("--",16,cx+19,483);
        else for(unsigned star=0;star<rating && star<10;++star)
            draw_text_at("★",11,cx+19,491-star*10);
    }
    draw_text_at("DON: CHOOSE CHART",17,x+(w-80)/2,238);
}

/* Confirmation timeline sampled from the supplied 60 Hz opening recording.
 * Keep this clock separate from row easing: catalog publications are frequent. */
static float menu_interval(double ms,double begin,double end)
{
    if(ms<=begin) return 0;
    if(ms>=end) return 1;
    return (float)((ms-begin)/(end-begin));
}

static int menu_open_contains(float x,float y,float inset)
{
    if(x<inset || x>420-inset || y<inset || y>560-inset) return 0;
    if(y>=64+inset) {
        float cx=x<20?20:x>400?400:x;
        float cy=y<84?84:y>540?540:y;
        float radius=20-inset;
        return (x-cx)*(x-cx)+(y-cy)*(y-cy)<=radius*radius;
    }
    /* Open containers have broader rounded shoulders than closed cards.
     * Keep the same slanted side and offset its border along the normal. */
    if(x<38 || x>382)return 0;
    // Shorter open flap: move its crown down four pixels, keeping the
    // shoulder/body junction fixed at y=64.
    if(y<4+inset)return 0;
    y=(y-4)*(64.0f/60.0f);
    x-=38;if(x>172)x=344-x;
    const float radius=20-inset;
    if(y<20-radius*0.177153f) {
        if(x>=30.36071f)return 1;
        float dx=x-30.36071f,dy=y-20;
        return dx*dx+dy*dy<=radius*radius;
    }
    return x>=13.63929f-y*0.18f+inset*1.016071f;
}

/* Four immutable slices keep the title centred as either wall moves. Scaling the complete
 * silhouette would stretch the title tab and its black outline. */
static void menu_shell_inset(const menu_folder_style* style,float top,float bottom,
                             float left,float right,float tab_inset,int heading)
{
    // Several categories can be visible simultaneously. Retain each immutable
    // style instead of rebuilding and freeing it on every neighbouring draw.
    static menu_art styles[12][4];
    unsigned slot=menu_style_index(style);
    menu_art* parts=styles[slot];
    if(!parts[0].pixels || !parts[1].pixels || !parts[2].pixels || !parts[3].pixels) {
        for(unsigned part=0;part<4;++part) { free(parts[part].pixels);parts[part].pixels=NULL; }
        const unsigned widths[]={20,1,344,20}, offsets[]={0,24,38,400};
        for(unsigned part=0;part<4;++part) {
            menu_art* a=&parts[part];
            a->width=widths[part]*2;a->height=560*2;
            a->pixels=calloc((size_t)a->width*a->height,4);
            if(!a->pixels) continue;
            for(unsigned y=0;y<a->height;++y) for(unsigned x=0;x<a->width;++x) {
                float px=x/2.0f+offsets[part]+0.25f,py=y/2.0f+0.25f;
                if(menu_open_contains(px,py,0))
                    a->pixels[y*a->width+x]=menu_open_contains(px,py,6)?style->colour:
                        (0xff000000u|((style->colour&0xfefefeu)>>1));
            }
        }
    }
    float height=bottom-top;
    uint64_t id=UINT64_C(0x4800000000000000)+slot*4;
    menu_bitmap(&parts[0],id,left,top,20,height,0);
    /* Disjoint spans are essential during fade-in: drawing a full-width body
     * under the tab would blend the middle twice and leave a dark rectangle. */
    float tab_left=468+tab_inset,tab_right=812-tab_inset;
    if(heading) {
        menu_bitmap(&parts[1],id+1,left+20,top,tab_left-left-20,height,0);
        menu_bitmap(&parts[2],id+2,tab_left,top,tab_right-tab_left,height,0);
        menu_bitmap(&parts[1],id+1,tab_right,top,right-20-tab_right,height,0);
    } else menu_bitmap(&parts[1],id+1,left+20,top,right-left-40,height,0);
    menu_bitmap(&parts[3],id+3,right-20,top,20,height,0);
}

static void menu_open_shell(const menu_folder_style* style,float top,float bottom,float left,float right)
{
    menu_shell_inset(style,top,bottom,left,right,0,1);
}

static float menu_folder_scroll(void)
{
    float t=menu_interval(monotonic_milliseconds()-g_folder_scroll_start,0,8000.0/60);
    t=1-(1-t)*(1-t);
    return g_folder_scroll_from+(g_folder_scroll_target-g_folder_scroll_from)*t;
}

/* Only the visible envelope grows. Absolute entry ordinals drive scrolling,
 * never the size of a polygon for a potentially enormous custom library. */
static float menu_folder_left(void)
{
    float left=430-menu_folder_scroll();
    return left < -96 ? -96 : left;
}

static float menu_folder_right(void)
{
    unsigned total=0;
    for(unsigned i=0;i<g_song_row_count;++i)
        if(g_song_rows[i].browser_total>total) total=g_song_rows[i].browser_total;
    if(g_song_shared_carousel)for(unsigned i=0;i<g_song_row_count;++i)
        if(g_song_rows[i].selected)total=g_song_rows[i].browser_total;
    if(!total) return 1376;
    float edge=850+(total-1)*96.0f-menu_folder_scroll();
    return edge>1376?1376:edge<850?850:edge;
}

static void menu_song_card(const song_row_storage* row,const menu_folder_style* style,
                           float x,float w,int selected,unsigned opacity)
{
    if(x+w<0 || x>1280) return;
    g_menu_alpha=g_text_opacity=opacity;
    static const menu_folder_style return_style={"Return",RGB_COLOUR(166,116,42),0,554};
    menu_folder(x,132,w,421,row->kind==TAIKO_OVERLAY_ROW_EXIT?&return_style:style,0);
    float opening=(w-76)/324;
    if(opening<0)opening=0;
    if(opening>1)opening=1;
    if(selected && opening>0)
        menu_yellow_frame(x,132,w,421);
    if(selected && opening>0) {
        float reveal=menu_interval(opening,0.3,0.7);
        g_menu_alpha=g_text_opacity=(unsigned)(opacity*reveal);
        if(row->kind==TAIKO_OVERLAY_ROW_EXIT) menu_image(421,476,275,246,246,0);
        else if(reveal>0) menu_song_courses(row,style,440,400);
    }
    g_menu_alpha=g_text_opacity=opacity;
    float centre=x+w/2+(selected?(row->kind==TAIKO_OVERLAY_ROW_EXIT?119:139)*opening:0);
    menu_spine(row->kind==TAIKO_OVERLAY_ROW_EXIT?"× Return":row->title,
               centre,row->kind==TAIKO_OVERLAY_ROW_EXIT?165:157,
               row->kind==TAIKO_OVERLAY_ROW_EXIT?335:360,
               selected || row->kind==TAIKO_OVERLAY_ROW_EXIT?0:menu_outline(style));
    g_menu_alpha=g_text_opacity=255;
}

/* Group bounds are inferred from a visible entry's local ordinal. Clamp the
 * relative positions before converting to pixels, regardless of library size. */
static float menu_bound_x(double relative,int right)
{
    if(relative < -7)return -96;
    if(relative > 7)return 1376;
    int rel=(int)relative;
    float value=menu_card_x(rel)+(right?menu_card_w(rel)+10:-10);
    return value < -96?-96:value>1376?1376:value;
}
static void menu_shared_target(const song_row_storage* row,int relative,float* left,float* right)
{
    *left=menu_bound_x((double)relative-row->browser_position,0);
    *right=menu_bound_x((double)relative+(row->browser_total?row->browser_total-1:0)-row->browser_position,1);
}
static void menu_shared_bounds(const song_row_storage* row,int relative,float ease,float* left,float* right)
{
    float target_left,target_right;
    menu_shared_target(row,relative,&target_left,&target_right);
    float t=ease/0.45f;if(t>1)t=1;
    *left=row->from_group_left+(target_left-row->from_group_left)*t;
    *right=row->from_group_right+(target_right-row->from_group_right)*t;
}

static void menu_category_contents(const song_row_storage* row,const menu_folder_style* style,unsigned opacity)
{
    char count[40];g_menu_alpha=g_text_opacity=opacity;
    g_menu_alpha=opacity/2;menu_image(492,463,150,159,37,0);g_menu_alpha=opacity;
    snprintf(count,sizeof count,"%u SONGS",row->catalog_index);
    draw_text_fit(count,23,149,542,169);
    menu_image(menu_character(style),468,208,180,320,0);
    draw_text_at("Play your",23,746,286);
    draw_text_at("favourite",23,746,321);
    draw_text_at("songs!",23,746,356);
    g_menu_alpha=g_text_opacity=255;
}

/* Paint a bounded mixed window. An open neighbour keeps its coloured body but
 * only the selected group has a raised title. No group closes on navigation. */
static void menu_shared_window(const song_row_storage* rows,unsigned count,int selected,
                               float ease,int neighbours,float left_shift,float right_shift)
{
    unsigned seen=0;
    for(unsigned i=0;i<count;++i) {
        const song_row_storage* row=&rows[i];
        if(!row->carousel_group || !row->browser_total)continue;
        unsigned bit=1u<<(row->carousel_group-1);
        if(seen&bit)continue;
        seen|=bit;
        int active=row->carousel_group==rows[selected].carousel_group;
        if(neighbours && active)continue;
        float left,right;
        menu_shared_bounds(row,(int)i-selected,ease,&left,&right);
        float shift=(int)i<selected?left_shift:right_shift;
        left+=shift;right+=shift;
        if(right<=0 || left>=1280)continue;
        if(left < -96)left=-96;if(right>1376)right=1376;
        const menu_folder_style* style=menu_style(row->genre);
        menu_shell_inset(style,54,573,left,right,0,active);
        if(active) {
            int outline=g_menu_text_outline;g_menu_text_outline=5;
            draw_text_fit(row->genre,36,300,640,90);
            g_menu_text_outline=outline;g_outline_radius=outline;
        }
    }
    for(unsigned pass=0;pass<2;++pass)for(unsigned i=0;i<count;++i) {
        const song_row_storage* row=&rows[i];
        int rel=(int)i-selected;
        if((!rel)!=(pass==1) || (neighbours && !rel))continue;
        float x,w;
        if(neighbours) {x=menu_card_x(rel);w=76;}
        else menu_card_pose(row->from_card_x,row->from_card_w,rel,ease,&x,&w);
        x+=rel<0?left_shift:rel>0?right_shift:0;
        if(x+w<0 || x>1280)continue;
        const menu_folder_style* style=menu_style(row->genre);
        if(row->kind!=TAIKO_OVERLAY_ROW_CATEGORY) {
            menu_song_card(row,style,x,w,rel==0,255);
            continue;
        }
        menu_folder(x,132,w,421,style,rel!=0 || w<=76);
        if(!rel && w>76)menu_heading(style,row->title,(w-76)/324,1);
        if(!rel && w>173) {
            float alpha=menu_interval((w-76)/324,0.3,0.7);
            menu_category_contents(row,style,(unsigned)(255*alpha));
        } else if(w<180)menu_spine(row->title,x+w/2,157,360,menu_outline(style));
    }
}

static void menu_close_folder(const menu_folder_style* active)
{
    double ms=monotonic_milliseconds()-g_folder_close_start;
    float fade=1-menu_interval(ms,0,5000.0/30);
    float contract=menu_interval(ms,5000.0/30,400);
    contract=1-(1-contract)*(1-contract);
    /* Both visible edges use one clock/easing curve, so unequal distances
     * finish together. The selected card and title never move horizontally. */
    float left=g_folder_close_left+(430-g_folder_close_left)*contract;
    float right=g_folder_close_right+(850-g_folder_close_right)*contract;
    float settle=menu_interval(ms,500,2000.0/3);
    float contents=menu_interval(ms,2000.0/3,800);
    if(g_song_shared_carousel) menu_shared_window(g_folder_categories,g_folder_category_count,
        g_folder_category_selected,1,1,left-430,right-850);
    else for(unsigned i=0;i<g_folder_category_count;++i) {
        int rel=(int)i-g_folder_category_selected;
        if(!rel)continue;
        float x=menu_card_x(rel)+(rel<0?left-430:right-850);
        if(x+76<0 || x>1280)continue;
        const menu_folder_style* style=menu_style(g_folder_categories[i].title);
        menu_folder(x,132,76,421,style,1);
        menu_spine(style->label,x+38,157,360,menu_outline(style));
    }
    /* Finish the geometry UNDER the foreground card and title tab. The shell
     * stays opaque: removing it is invisible only once every edge is covered. */
    menu_shell_inset(active,54+26*settle,573-24*settle,
                     left+14*settle,right-14*settle,6*settle,1);
    /* The selected card's teal bevel stays visible during the entire close. */
    menu_folder(440,132,400,421,active,0);
    if(fade>0) {
        int selected=0;
        for(unsigned i=0;i<g_folder_close_count;++i) if(g_folder_close_rows[i].selected)selected=i;
        uint8_t mask=g_song_difficulty_mask;g_song_difficulty_mask=g_folder_close_difficulties;
        for(unsigned i=0;i<g_folder_close_count;++i)
            if(!g_folder_close_group || g_folder_close_rows[i].carousel_group==g_folder_close_group)
            menu_song_card(&g_folder_close_rows[i],active,menu_card_x((int)i-selected),
                           menu_card_w((int)i-selected),(int)i==selected,(unsigned)(fade*255));
        g_song_difficulty_mask=mask;
    }
    if(settle>0) {
        g_menu_alpha=(unsigned)(settle*255);
        menu_heading(active,g_song_title,1,0);
        g_menu_alpha=255;
    }
    int outline=g_menu_text_outline;g_menu_text_outline=5;
    draw_text_fit(g_song_title,36+2*settle,300,640,90+16*settle);
    g_menu_text_outline=outline;g_outline_radius=outline;
    if(contents>0) {
        char count[40];
        g_menu_alpha=g_text_opacity=(unsigned)(255*contents);
        g_menu_alpha=g_text_opacity/2;menu_image(492,463,150,159,37,0);
        g_menu_alpha=g_text_opacity;
        snprintf(count,sizeof count,"%u SONGS",g_folder_categories[g_folder_category_selected].catalog_index);
        draw_text_fit(count,23,149,542,169);
        menu_image(menu_character(active),468,208,180,320,0);
        draw_text_at("Play your",23,746,286);
        draw_text_at("favourite",23,746,321);
        draw_text_at("songs!",23,746,356);
        g_menu_alpha=g_text_opacity=255;
    }
}


static void menu_open_folder(const menu_folder_style* active)
{
    double ms=monotonic_milliseconds()-g_folder_open_start;
    /* The reference holds the first pose for one 30 Hz source frame. The
     * black frame then vanishes; blue fades in over the next four frames.
     * Title glyph bounds stay constant throughout (there is no text zoom). */
    float lift=menu_interval(ms,1000.0/30,5000.0/30);
    float shell_alpha=lift;
    lift=1-(1-lift)*(1-lift);
    float settle=menu_interval(ms,5000.0/30,1000.0/3);
    settle=settle*settle*(3-2*settle);
    float spread=menu_interval(ms,400,2000.0/3);
    float reveal=menu_interval(ms,2000.0/3,2500.0/3);
    /* Constant acceleration in the horizontal opening, after the held pose. */
    float travel=480*spread*spread;
    float scroll=menu_folder_scroll();
    float left=menu_folder_left();
    float right=ms<2500.0/3?850+travel:menu_folder_right();
    if(g_song_shared_carousel) menu_shared_window(g_folder_categories,g_folder_category_count,
        g_folder_category_selected,1,1,-scroll,right-850);
    else for(unsigned i=0;i<g_folder_category_count;++i) {
        int rel=(int)i-g_folder_category_selected;
        if(!rel) continue;
        float x=menu_card_x(rel)+(rel>0?right-850:-scroll);
        if(x>1280 || x+76<0)continue;
        const menu_folder_style* style=menu_style(g_folder_categories[i].title);
        menu_folder(x,132,76,421,style,1);
        menu_spine(g_folder_categories[i].title,x+38,157,360,menu_outline(style));
    }
    g_menu_alpha=(unsigned)(255*shell_alpha);
    menu_open_shell(active,74-30*lift+10*settle,549+24*lift,
                    left+14*(1-lift),right-14*(1-lift));
    g_menu_alpha=255;
    if(ms+0.01<1000.0/30) {
        menu_folder(440,132,400,421,active,0);
        menu_heading(active,g_song_category,1,0);
    }
    g_menu_alpha=g_text_opacity=(unsigned)(255*(1-menu_interval(ms,1000.0/30,200)));
    if(g_menu_alpha) {
        char count[40];
        g_menu_alpha=g_text_opacity/2;
        menu_image(492,463,150,159,37,0);
        g_menu_alpha=g_text_opacity;
        snprintf(count,sizeof count,"%u SONGS",g_folder_categories[g_folder_category_selected].catalog_index);
        draw_text_fit(count,23,149,542,169);
        menu_image(menu_character(active),468,208,180,320,0);
        draw_text_at("Play your",23,746,286);
        draw_text_at("favourite",23,746,321);
        draw_text_at("songs!",23,746,356);
    }
    g_menu_alpha=g_text_opacity=255;
    int saved_outline=g_menu_text_outline;
    g_menu_text_outline=5;
    draw_text_fit(g_song_category,38-2*lift,300,640,106-26*lift+10*settle);
    g_menu_text_outline=saved_outline;g_outline_radius=saved_outline;
    if(reveal>0) {
        int selected=0;
        for(unsigned i=0;i<g_song_row_count;++i) if(g_song_rows[i].selected)selected=(int)i;
        g_menu_alpha=g_text_opacity=(unsigned)(255*reveal);
        for(unsigned i=0;i<g_song_row_count;++i) {
            const song_row_storage* row=&g_song_rows[i];
            if(g_song_shared_carousel && row->carousel_group!=g_song_rows[selected].carousel_group)continue;
            int rel=(int)i-selected;
            float x,w;
            if(ms<2500.0/3) { x=menu_card_x(rel);w=menu_card_w(rel); }
            else menu_card_pose(row->from_card_x,row->from_card_w,rel,song_ease(),&x,&w);
            if(x+w<0 || x>1280)continue;
            menu_song_card(row,active,x,w,rel==0,(unsigned)(255*reveal));
        }
        g_menu_alpha=g_text_opacity=255;
    }
}

/* Reference viewport: card body y=104..565, versus the previous 132..553.
 * Transform the entire carousel together so opening/closing silhouettes and
 * their contents remain registered throughout the approved animation. */
static HostUiEmit menu_layout_emit;
static void *menu_layout_user;
static void menu_carousel_emit(void *user,const HostUiDraw *source) {
    (void)user;
    HostUiDraw draw=*source;
    const float scale=461.0f/421.0f;
    draw.y=104+(draw.y-132)*scale;
    draw.h*=scale;
    menu_layout_emit(menu_layout_user,&draw);
}

static void render_green_categories(void)
{
    menu_load_art();
    const int categories=g_song_browser_level==TAIKO_OVERLAY_BROWSER_CATEGORIES;
    const menu_folder_style* active=menu_style(categories?g_song_title:g_song_category);
    unsigned bg=menu_background(active);
    fill_rect(0,0,1280,720,active->colour);
    /* Scroll at 24 logical pixels/second: one mirrored 1280px repeat
     * every 53 seconds. Absolute time keeps motion independent of FPS. */
    const double distance=monotonic_milliseconds()*0.024;
    const float scroll=(float)(distance-(uint64_t)(distance/1280.0)*1280.0);
    for(unsigned tile=0;tile<4;++tile)
        menu_image(bg,tile*640.0f-scroll,0,640,720,tile&1);
    if(!menu_image(244,16,12,340,77,0)) draw_text_at("SONG SELECT",42,178,44);
    fill_rounded_rect(980,28,1310,82,27,0xffffffffu);
    draw_text_at("TAB / CTRL+F SEARCH",20,1120,55);
    char label[96];
    /* The expanded folder's wide title tab is part of the folder silhouette. */
    if (!categories && !g_folder_open) {
        fill_rounded_rect(425,117,1280,568,14,active->colour);
    }

    menu_layout_emit=g_ui_emit;menu_layout_user=g_ui_user;
    if(g_ui_emit)g_ui_emit=menu_carousel_emit;
    int selected=0;
    for(unsigned i=0;i<g_song_row_count;++i) if(g_song_rows[i].selected) selected=(int)i;
    float ease=song_ease();
    if(g_folder_closing && monotonic_milliseconds()-g_folder_close_start>=800) g_folder_closing=0;
    if(g_folder_closing) menu_close_folder(active);
    else if (!categories && g_folder_open && (!g_song_shared_carousel ||
             monotonic_milliseconds()-g_folder_open_start<2500.0/3)) menu_open_folder(active);
    else if(g_song_shared_carousel) menu_shared_window(g_song_rows,g_song_row_count,selected,ease,0,0,0);
    else {
    /* Draw neighbours first so the expanding centre folder stays in front. */
    for(unsigned pass=0;pass<2;++pass) for(unsigned i=0;i<g_song_row_count;++i) {
        const song_row_storage* row=&g_song_rows[i];
        if((i==(unsigned)selected)!=(pass==1)) continue;
        int rel=(int)i-selected;
        float x,w;
        menu_card_pose(row->from_card_x,row->from_card_w,rel,ease,&x,&w);
        if(x+w<0 || x>1280) continue;
        const menu_folder_style* style=categories?menu_style(row->title):active;
        menu_folder(x,132,w,421,style,rel!=0 || w<=76);
        if(rel==0 && !categories && w>76) menu_yellow_frame(x,132,w,421);
        if(rel==0 && w>76)
            menu_heading(active,categories?g_song_title:g_song_category,(w-76)/324,1);
        if(rel==0 && w>173) {
            float opacity=((w-76)/324-0.3f)/0.4f;
            if(opacity>1) opacity=1;
            g_menu_alpha=g_text_opacity=(unsigned)(255*opacity);
            /* Content fades at its final position while the panel opens. */
            x=440;w=400;
            if(categories) {
                /* The original white pill is translucent over its category. */
                g_menu_alpha=g_text_opacity/2;
                menu_image(492,x+23,150,159,37,0);
                g_menu_alpha=g_text_opacity;
                snprintf(label,sizeof label,"%u SONGS",row->catalog_index);
                draw_text_fit(label,23,149,x+102,169);
                menu_image(menu_character(style),x+28,208,180,320,0);
                draw_text_at("Play your",23,x+w-94,286);
                draw_text_at("favourite",23,x+w-94,321);
                draw_text_at("songs!",23,x+w-94,356);
            } else {
                if(row->kind==TAIKO_OVERLAY_ROW_EXIT) {
                    menu_image(421,x+36,275,246,246,0);
                    menu_spine("Return",x+w-61,165,335,0);
                } else {
                    menu_spine(row->title,x+w-61,158,362,0);
                    menu_song_courses(row,active,x,w);
                }
            }
        } else if(w<180) menu_spine(row->kind==TAIKO_OVERLAY_ROW_EXIT?"Return":row->title,x+w/2,157,360,menu_outline(style));
        g_menu_alpha=g_text_opacity=255;
    }
    }
    if(!g_folder_closing) menu_navigation_arrows();
    g_ui_emit=menu_layout_emit;g_ui_user=menu_layout_user;
    fill_rect(0,606,1280,720,RGB_COLOUR(255,71,42));
    fill_rect(640,606,1280,720,RGB_COLOUR(100,190,192));
    /* Asset 394 has 18 transparent rows, then an eight-pixel divider.
     * Fallback panel colours must start BELOW that divider, not behind the
     * transparent padding (which exposed red/blue stripes above the line). */
    menu_image(394,0,580,1280,152,0);
    for(unsigned p=0;p<2;++p) {
        int joined=(g_browser_joined&(1u<<p))!=0;
        emit_portrait(p,0,255);
        float left=p?966:26;
        if(joined) {
            menu_nameplate(p,left,(g_browser_authenticated&(1u<<p))?g_browser_account_names[p]:"GUEST");
            snprintf(label,sizeof label,"%u  LEAVE PLAYER",p+1);
            draw_text_at(label,14,left+144,708);
        } else {
            draw_text_fit("HIT THE DRUM TO JOIN",22,288,left+144,664);
        }
    }
    menu_shadow(412,583,456,38,19,2);
    fill_rounded_rect(412,583,868,621,19,0xff000000);
    menu_control_drums();
    draw_text_at("Choose",21,551,602);
    draw_text_at("Confirm",21,773,602);
    draw_text_at("B  BANAPASSPORT LOGIN",18,640,646);
    draw_text_at(g_browser_save_status[0]?g_browser_save_status:"FREE PLAY",22,640,687);
    if(g_browser_login_phase) {
        fill_rounded_rect(388,204,892,490,16,0xff000000);
        fill_rounded_rect(394,210,886,484,12,RGB_COLOUR(255,235,157));
        draw_text_at("BANAPASSPORT",30,640,248);
        draw_text_left_fit(g_browser_login_status,20,440,420,295);
        if(g_browser_login_phase==1) {
            draw_text_at(g_code[0]?g_code:"------",52,640,360);
            draw_text_at("Enter this PIN on the pairing website",18,640,416);
        }
        draw_text_at("ESC  CANCEL",16,640,460);
    }
}
