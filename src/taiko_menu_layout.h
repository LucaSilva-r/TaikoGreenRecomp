#include "taiko_title_render.h"

/* Category-screen composition in Green's logical 1280x720 coordinates. */
static int green_categories(void)
{
    if (g_mode != 5) return 0;
    if (g_song_browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES) return 1;
    if (g_song_search_active || g_song_query[0] || !g_song_row_count) return 0;
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
    return relative < 0 ? 440 + relative*100 : relative > 0 ? 764 + relative*100 : 440;
}
static float menu_card_w(int relative) { return relative ? 76 : 400; }

static menu_art g_menu_middle[788];

static void menu_folder(float x, float y, float w, float h,
                         const menu_folder_style* style, int tab)
{
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
    float tab_width = w < 80 ? w : 80;
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
    if (tab && !menu_image(style->tab,x,y-14,tab_width,20,0))
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
    unsigned slot;
    for (slot=0; slot<16; ++slot)
        if (g_menu_spines[slot].art.pixels && !strcmp(text,g_menu_spines[slot].title) && g_menu_spines[slot].outline==outline) break;
    if (slot==16) {
        slot=g_menu_spine_next++%16;
        menu_art* art=&g_menu_spines[slot].art;
        free(art->pixels);
        art->width=56; art->height=400;
        art->pixels=calloc(56*400,sizeof(uint32_t));
        if (!art->pixels) return;
        if (!taiko_title_render_spine_argb(text,art->pixels,outline)) {
            free(art->pixels);art->pixels=NULL;return;
        }
        /* The guest title renderer returns ARGB words; host UI uses RGBA. */
        for (unsigned i=0;i<56*400;++i) {
            uint32_t c=art->pixels[i];
            art->pixels[i]=(c&0xff00ff00u)|((c>>16)&255)|((c&255)<<16);
        }
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

static void menu_control_drums(void)
{
    const float y=555, size=50;
    const int lit=((unsigned long)(monotonic_milliseconds()/600.0)&1)!=0;
    menu_bitmap(&g_menu_indicator[2],UINT64_C(0x4300000000000002),430,y,size,size,0);
    menu_bitmap(&g_menu_indicator[2],UINT64_C(0x4300000000000002),650,y,size,size,0);
    if(lit) {
        menu_bitmap(&g_menu_indicator[3],UINT64_C(0x4300000000000003),650,y,size,size,0);
        menu_bitmap(&g_menu_indicator[5],UINT64_C(0x4300000000000005),430,y,21.43f,size,0);
        menu_bitmap(&g_menu_indicator[5],UINT64_C(0x4300000000000005),458.57f,y,21.43f,size,1);
        menu_bitmap(&g_menu_indicator[6],UINT64_C(0x4300000000000006),415,y+15,20,20,0);
        menu_bitmap(&g_menu_indicator[6],UINT64_C(0x4300000000000006),475,y+15,20,20,1);
    }
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
    draw_text_fit(name,20,210,left+173,top+47);
}

static void render_green_categories(void)
{
    menu_load_art();
    const int categories=g_song_browser_level==TAIKO_OVERLAY_BROWSER_CATEGORIES;
    const menu_folder_style* active=menu_style(categories?g_song_title:g_song_category);
    unsigned bg=menu_background(active);
    fill_rect(0,0,1280,720,active->colour);
    /* Start with 12 logical pixels/second: one mirrored 1280px repeat
     * every 107 seconds. Absolute time keeps motion independent of FPS. */
    const double distance=monotonic_milliseconds()*0.012;
    const float scroll=(float)(distance-(uint64_t)(distance/1280.0)*1280.0);
    for(unsigned tile=0;tile<4;++tile)
        menu_image(bg,tile*640.0f-scroll,0,640,720,tile&1);
    if(!menu_image(244,16,12,264,60,0)) draw_text_at("SONG SELECT",36,164,42);
    draw_text_left_fit(categories?"CHOOSE A CATEGORY":"CHOOSE A SONG",18,400,25,89);
    fill_rounded_rect(923,26,1254,75,24,RGB_COLOUR(249,247,220));
    draw_text_at("TAB / CTRL+F  SEARCH",20,1088,51);
    char label[96];
    /* The expanded folder's wide title tab is part of the folder silhouette. */
    fill_rounded_rect(468,52,812,145,18,0xff000000);
    fill_rounded_rect(474,58,806,145,13,active->colour);
    draw_text_fit(categories?g_song_title:g_song_category,34,312,640,88);
    if (!categories) {
        fill_rounded_rect(425,117,1280,568,14,active->colour);
    }

    int selected=0;
    for(unsigned i=0;i<g_song_row_count;++i) if(g_song_rows[i].selected) selected=(int)i;
    float ease=song_ease();
    /* Draw neighbours first so the expanding centre folder stays in front. */
    for(unsigned pass=0;pass<2;++pass) for(unsigned i=0;i<g_song_row_count;++i) {
        const song_row_storage* row=&g_song_rows[i];
        if((i==(unsigned)selected)!=(pass==1)) continue;
        int rel=(int)i-selected;
        float x=row->from_card_x+(menu_card_x(rel)-row->from_card_x)*ease;
        float w=row->from_card_w+(menu_card_w(rel)-row->from_card_w)*ease;
        if(x+w<0 || x>1280) continue;
        const menu_folder_style* style=categories?menu_style(row->title):active;
        menu_folder(x,132,w,421,style,rel!=0);
        if(rel==0 && w>230) {
            if(categories) {
                fill_rounded_rect(x+23,150,x+182,187,18,RGB_COLOUR(255,238,180));
                snprintf(label,sizeof label,"%u SONGS",row->catalog_index);
                draw_text_fit(label,23,149,x+102,169);
                menu_image(menu_character(style),x+28,208,180,320,0);
                draw_text_at("Play your",23,x+w-94,286);
                draw_text_at("favourite",23,x+w-94,321);
                draw_text_at("songs!",23,x+w-94,356);
                draw_text_at(row->catalog_index?"DON TO OPEN":"NO SONGS",16,x+w-94,473);
            } else {
                menu_yellow_frame(x,132,w,421);
                if(row->kind==TAIKO_OVERLAY_ROW_EXIT) {
                    menu_image(421,x+36,275,246,246,0);
                    menu_spine("Return",x+w-61,165,335,0);
                } else {
                    menu_spine(row->title,x+w-61,158,362,0);
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
            }
        } else if(w<180) menu_spine(row->kind==TAIKO_OVERLAY_ROW_EXIT?"Return":row->title,x+w/2,157,360,menu_outline(style));
    }
    g_menu_alpha=112;
    menu_image(787,388,314,60,70,0);menu_image(787,832,314,60,70,1);
    g_menu_alpha=255;
    fill_rect(0,584,1280,720,RGB_COLOUR(255,71,42));
    fill_rect(640,584,1280,720,RGB_COLOUR(100,190,192));
    menu_image(394,0,568,1280,152,0);
    for(unsigned p=0;p<2;++p) {
        int joined=(g_browser_joined&(1u<<p))!=0;
        emit_portrait(p,0,255);
        float left=p?966:26;
        menu_nameplate(p,left,(g_browser_authenticated&(1u<<p))?g_browser_account_names[p]:joined?"GUEST":"HIT DRUM TO JOIN");
        snprintf(label,sizeof label,"%u  %s",p+1,joined?"LEAVE PLAYER":"JOIN PLAYER");
        draw_text_at(label,14,left+144,708);
    }
    fill_rounded_rect(412,558,868,603,22,0xff000000);
    menu_control_drums();
    draw_text_at("Choose",21,551,582);
    draw_text_at("Confirm",21,773,582);
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
