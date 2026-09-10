/* Category-screen composition in Green's logical 1280x720 coordinates. */
static int green_categories(void)
{
    return g_mode == 5 && g_song_browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES;
}

static int menu_image(unsigned id, float x, float y, float w, float h, int flip)
{
    if (id >= 788 || !g_menu_art[id].pixels || w <= 0 || h <= 0) return 0;
    const menu_art* art = &g_menu_art[id];
    if (g_ui_emit) {
        HostUiDraw d = {0};
        d.x=x; d.y=y; d.w=w; d.h=h; d.colour=0xffffffff;
        d.texture_id=UINT64_C(0x4000000000000000)+id;
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
            put_pixel(px,py,c,255);
        }
    }
    return 1;
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
    {"MEDLEY",RGB_COLOUR(255,107,97),657,592},
    {"CHILDREN'S SONGS",RGB_COLOUR(252,65,137),647,567},
    {"CUSTOM TJA",RGB_COLOUR(50,194,58),641,551},
    {"OSU! LAZER",RGB_COLOUR(252,65,137),673,567},
    {"NIJIIRO",RGB_COLOUR(255,107,97),655,592},
};
static const menu_folder_style* menu_style(const char* title)
{
    for(unsigned i=0;i<sizeof menu_styles/sizeof menu_styles[0];++i)
        if(!strcmp(title,menu_styles[i].label)) return &menu_styles[i];
    return &menu_styles[9];
}
static float menu_card_x(int relative)
{
    return relative < 0 ? 440 + relative*100 : relative > 0 ? 764 + relative*100 : 440;
}
static float menu_card_w(int relative) { return relative ? 76 : 400; }

static void menu_folder(float x,float y,float w,float h,const menu_folder_style* style)
{
    fill_rect(x+5,y+7,x+w+7,y+h+7,RGB_COLOUR(30,91,25));
    /* Original tab artwork is kept at its authored proportions. */
    if (!menu_image(style->tab,x,y-15,88,24,0))
        fill_rounded_rect(x,y-15,x+60,y+16,9,0xff000000);
    fill_rect(x,y,x+w,y+h,0xff000000);
    fill_rect(x+6,y+6,x+w-6,y+h-6,style->colour);
    menu_image(style->edge-1,x,y,22,h,0);
    menu_image(style->edge,x+w-22,y,22,h,0);
    /* Stretch the flat bevel between the original left/right corner strips. */
    uint32_t light=style->colour, dark=style->colour;
    for(unsigned shift=0;shift<24;shift+=8) {
        unsigned c=(style->colour>>shift)&255;
        light=(light&~(255u<<shift))|((c+(255-c)/3)<<shift);
        dark=(dark&~(255u<<shift))|((c*2/3)<<shift);
    }
    fill_rect(x+15,y+7,x+w-15,y+11,light);
    fill_rect(x+15,y+h-13,x+w-15,y+h-8,dark);
}

static void menu_spine(const char* text,float x,float y,float height)
{
    size_t length=strlen(text);
    float step=length>15 ? 22 : 26;
    float top=y+(height-length*step)/2+step/2;
    for(size_t i=0;i<length;++i) {
        char ch[2]={text[i],0};
        if(ch[0]!=' ') draw_text_at(ch,23,x,top+i*step);
    }
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

static void render_green_categories(void)
{
    menu_load_art();
    fill_rect(0,0,1280,720,RGB_COLOUR(57,194,58));
    menu_image(771,0,0,640,720,0); menu_image(771,640,0,640,720,1);
    if(!menu_image(244,16,12,264,60,0)) draw_text_at("SONG SELECT",36,164,42);
    draw_text_left_fit("CHOOSE A CATEGORY",18,400,25,89);
    fill_rounded_rect(923,26,1254,75,24,RGB_COLOUR(249,247,220));
    draw_text_at("TAB / CTRL+F  SEARCH",20,1088,51);
    char label[96];
    snprintf(label,sizeof label,"%u / %u",g_song_category_index+1,g_song_category_total);
    fill_rounded_rect(544,22,736,72,24,0xff000000);
    fill_rounded_rect(549,27,731,67,20,RGB_COLOUR(255,222,44));
    draw_text_at(label,25,640,47);

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
        const menu_folder_style* style=menu_style(row->title);
        menu_folder(x,132,w,421,style);
        if(rel==0 && w>230) {
            menu_image(87,x+w/2-36,182,72,84,0);
            menu_title(row->title,x+w/2,320,w-48);
            snprintf(label,sizeof label,"%u %s",row->catalog_index,row->catalog_index==1?"SONG":"SONGS");
            draw_text_at(label,25,x+w/2,407);
            draw_text_at(row->catalog_index?"DON TO OPEN":"NO SONGS INSTALLED",17,x+w/2,497);
        } else if(w<180) menu_spine(row->title,x+w/2,157,360);
    }
    menu_image(787,388,314,60,70,0);menu_image(787,832,314,60,70,1);
    fill_rect(0,584,1280,720,RGB_COLOUR(255,71,42));
    fill_rect(640,584,1280,720,RGB_COLOUR(100,190,192));
    menu_image(394,0,568,1280,152,0);
    for(unsigned p=0;p<2;++p) {
        int joined=(g_browser_joined&(1u<<p))!=0;
        emit_portrait(p,0,255);
        float left=p?966:26;
        fill_rounded_rect(left,636,left+288,690,26,0xff000000);
        fill_rounded_rect(left+4,640,left+284,686,23,p?RGB_COLOUR(185,237,240):RGB_COLOUR(255,183,171));
        if(!menu_image(90+p,left+3,637,52,52,0))
            draw_text_at(p?"2P":"1P",22,left+29,663);
        draw_text_left_fit((g_browser_authenticated&(1u<<p))?g_browser_account_names[p]:joined?"GUEST":"HIT DRUM TO JOIN",19,216,left+62,663);
        snprintf(label,sizeof label,"%u  %s",p+1,joined?"LEAVE PLAYER":"JOIN PLAYER");
        draw_text_at(label,14,left+144,708);
    }
    fill_rounded_rect(412,558,868,603,22,0xff000000);
    menu_image(195,430,559,38,44,0);draw_text_at("RIM: BROWSE",17,542,582);
    menu_image(196,646,559,38,44,0);draw_text_at("DON: OPEN",17,761,582);
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
