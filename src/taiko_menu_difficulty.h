#ifndef TAIKO_MENU_DIFFICULTY_H
#define TAIKO_MENU_DIFFICULTY_H
/* Green's "Choose Difficulty" screen, composed from the title's own Song
 * Select sprites at their authored pixel size. Geometry is measured from a
 * 1280x720 capture of the original screen.
 *
 * The dump is the Japanese release and the host UI is English, so every
 * authored label band is cleared at load (menu_blank_label) and only the
 * wording is generated here. Sprite identities:
 *   154/182/185/188/190  course columns (icon, wood frame, ten rating dots, drum)
 *   153/150/152          Back / Options / Sounds side tabs
 *   133/146  131/144     P1/P2 cursor outlines, rounded for tabs, square for columns
 *   140/148              P1/P2 balloons      155 filled rating star   158 crown
 *   115/113/112          option pane top cap, stretched middle, bottom cap
 *   114/116              option row, idle and selected     89  drum sound pane
 *   90/91                player badges       787 value arrow
 */

/* Course columns in Easy..Ura order. */
static const unsigned menu_course_art[5] = {154,182,185,188,190};

/* Sprite-local anchors inside a 104x584 course column. */
enum {
    MENU_COURSE_W = 104, MENU_COURSE_H = 584, MENU_COURSE_TOP = 90,
    MENU_COURSE_BODY = 51,          /* white body centre */
    MENU_COURSE_LABEL = 68,         /* cleared label band, top and bottom */
    MENU_COURSE_LABEL_END = 214,
    MENU_TAB_TOP = 111, MENU_TAB_W = 80, MENU_TAB_H = 320, MENU_TAB_PITCH = 70,
};
static const float menu_course_dot0 = 225.0f, menu_course_dot_step = 19.55f;
static const float menu_tab_x[3] = {245.0f, 315.0f, 385.0f};

static unsigned menu_glyph_count(const char* text)
{
    unsigned count = 0;
    for (const unsigned char* c=(const unsigned char*)text;*c;++c)
        if ((*c & 0xc0u) != 0x80u) ++count;
    return count;
}

/* One glyph per line, as the authored vertical labels are set. */
static void menu_vertical_text(const char* text,float x,float y,int size,float step,
                               uint32_t tint,int outline)
{
    const int saved_outline=g_menu_text_outline;
    const uint32_t saved_tint=g_menu_text_tint;
    g_menu_text_outline=outline;g_menu_text_tint=tint;
    unsigned line=0;
    for(const unsigned char* c=(const unsigned char*)text;*c;) {
        unsigned length=(*c&0x80u)==0?1:(*c&0xe0u)==0xc0u?2:(*c&0xf0u)==0xe0u?3:4;
        char glyph[5];unsigned taken=0;
        while(taken<length && c[taken]) {glyph[taken]=(char)c[taken];++taken;}
        glyph[taken]=0;
        draw_text_at(glyph,size,x,y+line*step);
        c+=taken;++line;
    }
    g_menu_text_outline=saved_outline;g_menu_text_tint=saved_tint;
    g_outline_radius=saved_outline;
}

static void menu_fit_text(const char* text,int size,int width,float x,float y,
                          uint32_t tint,int outline,int left_aligned)
{
    const int saved_outline=g_menu_text_outline;
    const uint32_t saved_tint=g_menu_text_tint;
    g_menu_text_outline=outline;g_menu_text_tint=tint&0xffffffu;
    if(left_aligned) draw_text_left_fit(text,size,width,x,y);
    else draw_text_fit(text,size,width,x,y);
    g_menu_text_outline=saved_outline;g_menu_text_tint=saved_tint;
    g_outline_radius=saved_outline;
}

/* The rasterizer's outline is always black and the GPU path tints a whole
 * cached quad with one colour, so a coloured ring has to be stamped as eight
 * offset copies under the fill. Reserve it for the one headline. */
static void menu_ringed_text(const char* text,int size,int width,float x,float y,
                             uint32_t ring,int radius,uint32_t fill,int left_aligned)
{
    static const int dx[8]={-1,0,1,-1,1,-1,0,1},dy[8]={-1,-1,-1,0,0,1,1,1};
    /* Outlined stamps first for the black edge, then flat stamps so a later
     * neighbour's edge cannot cut into the ring, then the fill. */
    for(unsigned pass=0;pass<2;++pass)
        for(unsigned i=0;i<8;++i)
            menu_fit_text(text,size,width,x+dx[i]*radius,y+dy[i]*radius,
                          ring,pass?0:3,left_aligned);
    menu_fit_text(text,size,width,x,y,fill,0,left_aligned);
}

/* Option identities match BrowserPlayers::options; index 5 belongs to the
 * drum sound pane and never appears in the game options list. */
static const char* const menu_option_names[6] =
    {"Classic Score","Speed","Stealth","Invert","Random","Drum Sound"};
static const char* const menu_option_off_on[2] = {"OFF","ON"};
static const char* const menu_option_speed[4] = {"Normal","x2","x3","x4"};
static const char* const menu_option_random[3] = {"OFF","Kimagure","Detarame"};
static const char* const menu_option_drum[4] = {"Normal","Type 2","Type 3","Type 4"};

static const char* menu_option_value(unsigned option,unsigned value)
{
    switch(option) {
    case 1: return menu_option_speed[value<4?value:0];
    case 4: return menu_option_random[value<3?value:0];
    case 5: return menu_option_drum[value<4?value:0];
    default: return menu_option_off_on[value<2?value:0];
    }
}

/* Idle/selected row sprites are 400x56 with a 380x44 body and a baked value
 * pill; both are drawn at their authored size. */
static void menu_option_row(float x,float y,const char* name,const char* value,
                            int selected)
{
    menu_image(selected?116:114,x,y,400,56,0);
    if(name[0]) menu_fit_text(name,26,150,x+59,y+27,0,1,1);
    if(!value) return;
    menu_fit_text(value,28,150,x+292,y+27,0,1,0);
    if(!selected) return;
    menu_image(787,x+185,y+13,26,30,0);
    menu_image(787,x+360,y+13,26,30,1);
}

static void menu_option_pane(unsigned player,unsigned pane,unsigned row,
                             const uint8_t values[6])
{
    const float base=player?832.0f:-16.0f;
    const float ease=menu_interval(monotonic_milliseconds()-g_difficulty_pane_start[player],
                                   0,180);
    const float slide=(1-ease)*36;
    const unsigned saved_alpha=g_menu_alpha,saved_opacity=g_text_opacity;
    g_menu_alpha=g_text_opacity=(unsigned)(255*ease);
    if(pane==2) {
        menu_image(89,base,354+slide,464,408,0);
    } else {
        menu_image(115,base,347+slide,464,96,0);
        menu_image(113,base,443+slide,464,176,0);
        menu_image(112,base,619+slide,464,128,0);
    }
    menu_image(player?91:90,base+101,400+slide,40,40,0);
    draw_text_fit(pane==2?"Drum Sound":"Game Options",34,268,base+306,420+slide);
    if(pane==2) {
        /* The header already names it; the row is only the value picker. */
        menu_option_row(base+37,445+slide,"",menu_option_value(5,values[5]),1);
    } else for(unsigned i=0;i<5;++i)
        menu_option_row(base+37,445+slide+i*50,menu_option_names[i],
                        menu_option_value(i,values[i]),row==i);
    g_menu_alpha=saved_alpha;g_text_opacity=saved_opacity;
}

/* Panel pose and content opacity over the enter/leave timelines, taken frame
 * by frame from the reference capture at 30 Hz and all linear:
 *   enter  spines exit by 333ms, hold to 533ms, widen to 783ms,
 *          raise to 1000ms, contents 1033..1200ms, heading 1300..1433ms
 *   leave  fade contents out 6f (200 ms), lower 7f (233 ms), narrow 8f (267 ms)
 * The panel grows out of, and shrinks back into, the selected song card, so
 * both ends of the move are the card geometry the browser itself draws. */
typedef struct menu_difficulty_pose {
    float x,y,w,h,wide,contents;
    int finished;
} menu_difficulty_pose;

static menu_difficulty_pose menu_difficulty_timeline(void)
{
    menu_difficulty_pose pose;
    const double now=monotonic_milliseconds();
    float tall;
    pose.finished=0;
    if(g_difficulty_closing) {
        const double e=now-g_difficulty_close_start;
        pose.contents=1-menu_interval(e,0,200);
        tall=1-menu_interval(e,200,433);
        pose.wide=1-menu_interval(e,433,700);
        pose.finished=e>=700;
    } else {
        const double e=now-g_difficulty_start;
        pose.wide=menu_interval(e,533,783);
        tall=menu_interval(e,783,1000);
        pose.contents=menu_interval(e,1033,1200);
    }
    const float card_w=g_difficulty_closing?menu_selected_width():g_difficulty_card_w;
    const float card_x=g_difficulty_closing?640-card_w/2:g_difficulty_card_x;
    pose.x=card_x+(240-card_x)*pose.wide;
    pose.w=card_w+(800-card_w)*pose.wide;
    pose.y=104+(44-104)*tall;
    pose.h=461+(520-461)*tall;
    return pose;
}

/* Frames have to keep coming while either timeline runs. */
static int menu_difficulty_busy(void)
{
    return g_difficulty_closing || g_difficulty_open;
}

/* Course changes trigger one bottom-anchored stretch. Confirmation preserves the
 * pose and size, flashes the silhouette and reveals the course name. */
static void menu_difficulty_emblems(float contents)
{
    static const unsigned art[5]={227,224,221,220,220};
    static unsigned previous_course[2]={~0u,~0u};
    static double changed_at[2],screen_start=-1;
    if(screen_start!=g_difficulty_start) {
        screen_start=g_difficulty_start;
        previous_course[0]=previous_course[1]=~0u;
    }
    static const unsigned flash[5]={228,225,222,0,0};
    static const float ink_bottom[5]={299,313,306,339,339};
    static const char* const names[5]={"Easy","Normal","Hard","Extreme","Ura"};
    const double now=monotonic_milliseconds();
    const unsigned alpha=g_menu_alpha,opacity=g_text_opacity;
    for(unsigned p=0;p<2;++p) {
        if(!(g_browser_joined&(1u<<p))) continue;
        unsigned course=g_browser_difficulties[p];
        /* Published rows also support chart cursors in the standalone preview. */
        const song_row_storage* rows=g_difficulty_closing?g_difficulty_close_rows:g_song_rows;
        const unsigned count=g_difficulty_closing?g_difficulty_close_count:g_song_row_count;
        for(unsigned i=0;i<count;++i)
            if(rows[i].kind==TAIKO_OVERLAY_ROW_DIFFICULTY && (rows[i].cursors&(1u<<p)))
                course=rows[i].difficulty;
        if(course>4) course=3;
        const int ready=(g_browser_ready&(1u<<p))!=0;
        const double elapsed=now-g_browser_confirm_start[p];
        const float confirm=ready?menu_interval(elapsed,0,300):0;
        if(previous_course[p]!=course) {
            /* The first published course establishes the resting pose. */
            changed_at[p]=previous_course[p]==~0u?now-500:now;
            previous_course[p]=course;
        }
        const float t=menu_interval(now-changed_at[p],0,450);
        const float stretch=(float)sin(t*3.141592654);
        const float w=360*(1-0.10f*stretch),h=360*(1+0.14f*stretch);
        const float cx=p?1160:120;
        /* Anchor the visible artwork, excluding transparent atlas padding. */
        const float y=170+ink_bottom[course]*(1-h/360);
        g_menu_alpha=(unsigned)(contents*(90+165*confirm));
        menu_image(art[course],cx-w/2,y,w,h,0);
        if(ready && flash[course] && elapsed<300) {
            g_menu_alpha=(unsigned)(contents*255*(1-menu_interval(elapsed,0,300)));
            menu_image(flash[course],cx-w/2,y,w,h,0);
        }
        if(ready) {
            static uint32_t gradient_pixels[256];
            static menu_art gradient;
            if(!gradient.pixels) {
                gradient.width=256;gradient.height=1;gradient.pixels=gradient_pixels;
                for(unsigned x=0;x<256;++x) {
                    const float strength=1-fabsf((x-127.5f)/127.5f);
                    gradient_pixels[x]=((unsigned)(150*strength)<<24)|0xffffffu;
                }
            }
            g_text_opacity=(unsigned)(255*contents*menu_interval(elapsed,300,450));
            g_menu_alpha=g_text_opacity;
            menu_bitmap(&gradient,UINT64_C(0x4900000000000000),cx-120,165,240,60,0);
            menu_fit_text(names[course],60,230,cx,195,0xffffff,3,0);
        }
    }
    g_menu_alpha=alpha;g_text_opacity=opacity;
}

/* Retarget from the current displayed position, including rapid Ka input. */
static float menu_cursor_position(unsigned player,unsigned axis,float target)
{
    static float from[2][2],to[2][2];
    static double start[2][2],screen[2][2];
    const double now=monotonic_milliseconds();
    float t=menu_interval(now-start[player][axis],0,110);
    float value=from[player][axis]+(to[player][axis]-from[player][axis])*t;
    if(screen[player][axis]!=g_difficulty_start) {
        screen[player][axis]=g_difficulty_start;
        from[player][axis]=to[player][axis]=target;value=target;
    }
    if(to[player][axis]!=target) {
        from[player][axis]=value;to[player][axis]=target;start[player][axis]=now;
    }
    return value;
}

/* Returns 0 once a close has played out and the browser owns the frame. */
static int render_green_difficulty(void)
{
    const menu_difficulty_pose pose=menu_difficulty_timeline();
    if(pose.finished) { g_difficulty_closing=0; return 0; }
    const menu_folder_style* style=menu_style(g_song_category);
    fill_rect(0,0,1280,720,style->colour);
    const double distance=monotonic_milliseconds()*0.024;
    const float scroll=(float)(distance-(uint64_t)(distance/1280.0)*1280.0);
    for(unsigned tile=0;tile<4;++tile)
        menu_image(menu_background(style),tile*640.0f-scroll,0,640,720,tile&1);

    const double entering=monotonic_milliseconds()-g_difficulty_start;
    if(!g_difficulty_closing && entering<600) {
        HostUiEmit saved_emit=g_ui_emit;
        void* saved_user=g_ui_user;
        menu_layout_emit=saved_emit;menu_layout_user=saved_user;
        if(saved_emit)g_ui_emit=menu_carousel_emit;
        int selected=0;
        for(unsigned i=0;i<g_difficulty_enter_count;++i)
            if(g_difficulty_enter_rows[i].selected)selected=i;
        g_menu_alpha=g_text_opacity=(unsigned)(255*(1-menu_interval(entering,0,133)));
        float left=-96,right=1376;
        const float saved_width=g_menu_width_override;
        g_menu_width_override=g_difficulty_card_w;
        if(g_difficulty_enter_count && g_difficulty_enter_rows[selected].carousel_group)
            menu_shared_bounds(&g_difficulty_enter_rows[selected],0,g_difficulty_enter_ease,&left,&right);
        menu_open_shell(style,54,573,left,right);
        draw_text_fit(g_song_category,36,300,640,90);
        for(unsigned i=0;i<g_difficulty_enter_count;++i) {
            const int rel=(int)i-selected;
            if(!rel)continue;
            const float travel=menu_interval(entering,0,333)*640;
            menu_song_card(&g_difficulty_enter_rows[i],menu_style(g_difficulty_enter_rows[i].genre),
                           menu_card_x(rel)+(rel<0?-travel:travel),76,0,255);
        }
        g_menu_width_override=saved_width;
        g_ui_emit=saved_emit;g_ui_user=saved_user;
        g_menu_alpha=g_text_opacity=255;
    }
    menu_difficulty_emblems(pose.contents);
    menu_yellow_frame(pose.x,pose.y,pose.w,pose.h);
    if(!g_difficulty_closing && entering<133 && g_difficulty_enter_count) {
        for(unsigned i=0;i<g_difficulty_enter_count;++i)if(g_difficulty_enter_rows[i].selected) {
            g_menu_alpha=g_text_opacity=(unsigned)(255*(1-menu_interval(entering,0,133)));
            HostUiEmit saved_emit=g_ui_emit;
            void* saved_user=g_ui_user;
            menu_layout_emit=saved_emit;menu_layout_user=saved_user;
            if(saved_emit)g_ui_emit=menu_carousel_emit;
            menu_song_courses(&g_difficulty_enter_rows[i],style,pose.x,pose.w);
            g_ui_emit=saved_emit;g_ui_user=saved_user;
        }
        g_menu_alpha=g_text_opacity=255;
    }
    if(!g_difficulty_closing && entering<600) {
        g_menu_alpha=(unsigned)(255*(1-menu_interval(entering,500,600)));
        menu_image(244,16,12,340,77,0);
        g_menu_alpha=255;
    }
    /* The spine rides the panel so it lands on the card's own spine. */
    menu_spine(g_song_title,pose.x+pose.w-61-27*pose.wide,pose.y+25*461.0f/421.0f,360*461.0f/421.0f,0);
    if(pose.contents<=0) { menu_bottom_bar(); return 1; }
    const unsigned fade=(unsigned)(255*pose.contents);
    g_menu_alpha=g_text_opacity=fade;
    /* The authored headline, at its own size and place. The generated text
     * is only a fallback for a tree with no Song Select archive. */
    if(!g_difficulty_closing)
        g_menu_alpha=g_text_opacity=(unsigned)(255*menu_interval(entering,1300,1433));
    if(!menu_image(192,6,9,352,80,0))
        menu_ringed_text("Choose Difficulty",40,340,14,46,
                         RGB_COLOUR(237,50,35),4,0xffffffu,1);
    g_menu_alpha=g_text_opacity=fade;

    static const char* const tab_labels[3]={"Back","Options","Sounds"};
    static const unsigned tab_art[3]={153,150,152};
    for(unsigned t=0;t<3;++t) {
        menu_image(tab_art[t],menu_tab_x[t],MENU_TAB_TOP,MENU_TAB_W,MENU_TAB_H,0);
        const float step=26;
        menu_vertical_text(tab_labels[t],menu_tab_x[t]+40,
                           MENU_TAB_TOP+82,28,step,0xffffffu,4);
    }

    const song_row_storage* rows=g_difficulty_closing?g_difficulty_close_rows:g_song_rows;
    const unsigned row_count=g_difficulty_closing?g_difficulty_close_count:g_song_row_count;
    unsigned column_row[TAIKO_OVERLAY_SONG_ROW_COUNT];
    unsigned columns=0;
    for(unsigned i=0;i<row_count;++i)
        if(rows[i].kind==TAIKO_OVERLAY_ROW_DIFFICULTY)
            column_row[columns++]=i;
    /* A shared four-slot viewport follows the player who last navigated.
     * Absolute chart positions keep streamed osu windows stable. */
    static int window=0,old_window=0;
    static double scroll_start=0,screen=-1;
    static uint32_t navigation_serial;
    unsigned positions[TAIKO_OVERLAY_SONG_ROW_COUNT],total=columns;
    int cursor[2]={-1,-1};
    for(unsigned c=0;c<columns;++c) {
        const song_row_storage* row=&rows[column_row[c]];
        positions[c]=row->browser_total?row->browser_position:c;
        if(row->browser_total)total=row->browser_total;
        for(unsigned p=0;p<2;++p)if(row->cursors&(1u<<p))cursor[p]=positions[c];
    }
    const double now=monotonic_milliseconds();
    if(screen!=g_difficulty_start) {
        screen=g_difficulty_start;window=old_window=0;scroll_start=now-600;
        navigation_serial=g_difficulty_menu.navigation_serial;
    }
    if(navigation_serial!=g_difficulty_menu.navigation_serial) {
        // Complete the previous movement before handling this Ka press.
        old_window=window;scroll_start=now-250;
        navigation_serial=g_difficulty_menu.navigation_serial;
    }
    int target=window;
    const unsigned focus=g_difficulty_menu.focus&1;
    if(cursor[focus]>=0 && !(g_browser_ready&(1u<<focus))) {
        if(cursor[focus]<target)target=cursor[focus];
        if(cursor[focus]>=target+4)target=cursor[focus]-3;
    }
    if(target<0)target=0;
    if(target>(int)total-4)target=total>4?total-4:0;
    if(target!=window) {old_window=window;window=target;scroll_start=now;}
    float slide=menu_interval(now-scroll_start,0,250);
    float departure=slide,arrival=slide;
    /* Drop, slide and rise together, with smooth acceleration and settling. */
    slide=slide*slide*(3-2*slide);
    departure=departure*departure*(3-2*departure);
    arrival=arrival*arrival*(3-2*arrival);
    float centres[TAIKO_OVERLAY_SONG_ROW_COUNT],drops[TAIKO_OVERLAY_SONG_ROW_COUNT];
    int visible[TAIKO_OVERLAY_SONG_ROW_COUNT];
    for(unsigned c=0;c<columns;++c) {
        const int pos=positions[c];
        const int was=pos>=old_window && pos<old_window+4;
        const int is=pos>=window && pos<window+4;
        visible[c]=is || (was && departure<1);
        const float centre=553+100*(pos-(old_window+(window-old_window)*slide));
        centres[c]=centre;
        drops[c]=!is?530*departure:!was?530*(1-arrival):0;
        if(!visible[c])continue;
        const float drop=drops[c];
        const song_row_storage* row=&rows[column_row[c]];
        g_menu_alpha=g_text_opacity=(unsigned)(fade*(1-drop/530));
        /* Two unearned crown places, ghosted: the silver sprite's white body
         * disappears into the panel and leaves its outline, which is what the
         * reference shows. A filled black crown would read as a solid blob. */
        const unsigned saved_alpha=g_menu_alpha;
        g_menu_alpha=(unsigned)(90*pose.contents*(1-drop/530));
        menu_image(159,centre-37.6f,62.5f+drop,44.3f,35.6f,0);
        menu_image(159,centre-6.6f,62.5f+drop,44.3f,35.6f,0);
        g_menu_alpha=saved_alpha;
        const unsigned course=row->difficulty<5?row->difficulty:3;
        menu_image(menu_course_art[course],centre-MENU_COURSE_W/2,MENU_COURSE_TOP+drop,
                   MENU_COURSE_W,MENU_COURSE_H,0);
        const float body=centre-MENU_COURSE_W/2+MENU_COURSE_BODY;
        /* The catalog names courses in caps; the authored screen sets them
         * in title case. Osu chart names keep whatever they were given. */
        static const char* const stock[5]={"EASY","NORMAL","HARD","ONI","URA"};
        static const char* const cased[5]={"Easy","Normal","Hard","Extreme","Ura"};
        const char* label=row->title;
        if(!strcmp(label,stock[course])) label=cased[course];
        /* Keep short names tightly stacked and long names within the same
         * label band. A half-opacity stroke gives an intermediate weight. */
        const unsigned glyphs=menu_glyph_count(label);
        const float step=glyphs>5?128.0f/glyphs:24.0f;
        g_menu_text_scale_x=0.90f;g_menu_text_scale_y=0.80f;
        const unsigned label_opacity=g_text_opacity;
        g_text_opacity=label_opacity/2;
        menu_vertical_text(label,body,
                           MENU_COURSE_TOP+MENU_COURSE_LABEL+12+step/2+drop,
                           (int)(step*1.5f),step,0,1);
        g_text_opacity=label_opacity;
        menu_vertical_text(label,body,
                           MENU_COURSE_TOP+MENU_COURSE_LABEL+12+step/2+drop,
                           (int)(step*1.5f),step,0,0);
        g_menu_text_scale_x=g_menu_text_scale_y=1;
        for(unsigned star=0;star<row->stars && star<10;++star) {
            const float y=MENU_COURSE_TOP+drop+menu_course_dot0+
                (9-star)*menu_course_dot_step;
            menu_image(155,body-20,y-20,40,40,0);
        }
    }

    g_menu_alpha=g_text_opacity=fade;
    /* Cursors. A player on a side tab has a negative item; otherwise the
     * frontend already marked the column that player's cursor sits on. */
    /* The legacy diagnostic path has no joined players but still marks the
     * guest's current course, so a published cursor counts as present. */
    unsigned present=g_browser_joined;
    for(unsigned c=0;c<columns;++c) present|=rows[column_row[c]].cursors;
    /* Resolve both animated positions before drawing either player. A shared
     * logical selection is merged only once both cursors reach the column. */
    float cursor_x[2]={0,0},cursor_y[2]={0,0};
    int cursor_settled[2]={0,0};
    for(unsigned p=0;p<2;++p) {
        if(!(present&(1u<<p)))continue;
        const int item=g_difficulty_menu.item[p];
        float target_x,target_y;
        if(item<0) {
            unsigned tab=(unsigned)(item+3)<3?(unsigned)(item+3):0;
            target_x=menu_tab_x[tab]+40;target_y=MENU_TAB_TOP+6-131;
        } else {
            unsigned c=0;
            while(c<columns && !(rows[column_row[c]].cursors&(1u<<p)))++c;
            if(c>=columns || !visible[c] || drops[c]>1)continue;
            target_x=centres[c];target_y=6;
        }
        cursor_x[p]=menu_cursor_position(p,0,target_x);
        cursor_y[p]=menu_cursor_position(p,1,target_y);
        cursor_settled[p]=fabsf(cursor_x[p]-target_x)<0.01f &&
                          fabsf(cursor_y[p]-target_y)<0.01f;
    }
    for(unsigned p=0;p<2;++p) {
        if(!(present&(1u<<p))) continue;
        if((g_browser_ready&(1u<<p)) &&
           monotonic_milliseconds()-g_browser_confirm_start[p]>=200) continue;
        const int item=g_difficulty_menu.item[p];
        float tip_x,balloon_y;
        int shared_course=0;
        if(item<0) {
            const unsigned t=(unsigned)(item+3)<3?(unsigned)(item+3):0;
            tip_x=cursor_x[p];
            balloon_y=MENU_TAB_TOP+6-131;
            menu_image(p?146:133,tip_x-40,MENU_TAB_TOP,MENU_TAB_W,MENU_TAB_H,0);
        } else {
            unsigned c=0;
            while(c<columns && !(rows[column_row[c]].cursors&(1u<<p))) ++c;
            if(c>=columns) continue;
            if(!visible[c] || drops[c]>1) {
                const float edge=positions[c]<(unsigned)window?490:920;
                menu_image(p?148:140,edge-26,420+p*48,52,68,0);
                menu_fit_text(rows[column_row[c]].title,18,115,edge,500+p*40,0xffffff,2,0);
                continue;
            }
            tip_x=cursor_x[p];
            shared_course=cursor_settled[0] && cursor_settled[1] &&
                (rows[column_row[c]].cursors&3)==3 && !(g_browser_ready&3) &&
                g_difficulty_menu.item[0]>=0 && g_difficulty_menu.item[1]>=0;
            /* The coloured stroke extends under the course icon and below
             * the last star, ending just above the drum. The sprite's stroke
             * spans y 19..379: this places it at y 151..513. */
            if(shared_course) {
                /* The split sprite has no transparent padding, unlike the
                 * individual outlines. Match their visible stroke bounds. */
                if(p==0) menu_image(135,tip_x-33.8f,150,70.6f,365,0);
            } else menu_image(p?144:131,tip_x-39.7f,131.9f,78.4f,402.2f,0);
            /* Its point rests on the wood frame's top, overlapping the icon. */
            balloon_y=6;
        }
        balloon_y=cursor_y[p];
        /* One-player selection uses the coloured border alone. */
        if((present&3)!=3)continue;
        const unsigned saved_alpha=g_menu_alpha;
        if(g_browser_ready&(1u<<p))
            g_menu_alpha=(unsigned)(g_menu_alpha*(1-menu_interval(
                monotonic_milliseconds()-g_browser_confirm_start[p],0,200)));
        if(g_difficulty_menu.pane[p]) g_menu_alpha=(unsigned)(90*pose.contents);
        if(shared_course)
            menu_image(p?149:142,tip_x-100,balloon_y,200,136,0);
        else menu_image(p?148:140,tip_x-52,balloon_y,104,136,0);
        g_menu_alpha=saved_alpha;
    }

    g_menu_alpha=g_text_opacity=255;
    menu_bottom_bar();
    /* The panes overlap the player panels, so they come after the bar. */
    g_menu_alpha=g_text_opacity=fade;
    for(unsigned p=0;p<2;++p)
        if(g_difficulty_menu.pane[p])
            menu_option_pane(p,g_difficulty_menu.pane[p],
                             g_difficulty_menu.option_row[p],
                             g_difficulty_menu.values[p]);
    g_menu_alpha=g_text_opacity=255;
    return 1;
}
#endif
