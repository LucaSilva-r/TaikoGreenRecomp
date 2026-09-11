/* Standalone host UI renderer smoke/preview; no guest or game dump required. */
#include "taiko_overlay.h"
#include "rsx_sdl_gpu_backend.h"
#include <ps3emu/host_sdl.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
double taiko_preview_clock_ms = -1;
static int opening_restart, opening_pause, opening_step;
static void opening_key(int key)
{
    if(key==SDL_SCANCODE_R || key==SDL_SCANCODE_RETURN) opening_restart=1;
    if(key==SDL_SCANCODE_SPACE) opening_pause=!opening_pause;
    if(key==SDL_SCANCODE_LEFT) { opening_pause=1;opening_step=-1; }
    if(key==SDL_SCANCODE_RIGHT) { opening_pause=1;opening_step=1; }
}
static void opening_publish(int inside)
{
    static const char* folders[]={"MEDLEY","CHILDREN'S SONGS","CUSTOM TJA","OSU! LAZER","NIJIIRO","J-POP","ANIME","VOCALOID","VARIETY","CLASSICAL","GAME MUSIC"};
    static const char* songs[]={"Return","馬と鹿","Lemon","LOSER","剣乱舞","Another song","Music","Next song","Song nine","Song ten","Return"};
    taiko_overlay_song_row rows[TAIKO_OVERLAY_SONG_ROW_COUNT]={0};
    for(unsigned i=0;i<TAIKO_OVERLAY_SONG_ROW_COUNT;++i) {
        rows[i].title=inside?songs[i]:folders[i];
        rows[i].genre=inside?"J-POP":folders[i];
        rows[i].kind=inside?(i==0 || i==10?TAIKO_OVERLAY_ROW_EXIT:TAIKO_OVERLAY_ROW_SONG):TAIKO_OVERLAY_ROW_CATEGORY;
        rows[i].catalog_index=inside?i:84;
        rows[i].browser_position=i;rows[i].browser_total=TAIKO_OVERLAY_SONG_ROW_COUNT;
        rows[i].selected=i==(inside?0:5);
    }
    taiko_overlay_show_song_browser("P1","",inside?"Return":"J-POP",inside?"J-POP":"CATEGORY FOLDER",84,0,84,881,inside?"J-POP":"CATEGORIES",0,12,"",0,"",0,inside?TAIKO_OVERLAY_BROWSER_SONGS:TAIKO_OVERLAY_BROWSER_CATEGORIES,inside,rows,TAIKO_OVERLAY_SONG_ROW_COUNT);
}
static int opening_preview(int argc,char** argv)
{
    int sequence=!strcmp(argv[4],"opening-frames");
    int still=sequence || !strcmp(argv[4],"opening-frame");
    double frame=argc>5?strtod(argv[5],NULL):50;
    Uint64 last=SDL_GetTicks(),start=last;
    double elapsed=0;
    int inside=0;
    taiko_overlay_set_browser_players(1,1,0,NULL);
    taiko_preview_clock_ms=10000;opening_publish(0);
    g_rsx_replay_key_hook=opening_key;
    fprintf(stderr,"Opening preview: Enter/R restart; Space pause; arrows step at 60 Hz.\n");
    if(still) {
        taiko_preview_clock_ms=11000;opening_publish(1);
        unsigned count=sequence?(argc>5?strtoul(argv[5],NULL,10):54):1;
        for(unsigned i=0;i<count;++i) {
            char file[4096];
            taiko_preview_clock_ms=11000+(sequence?i:frame)*1000/60;
            int n=sequence?snprintf(file,sizeof file,"%s-%03u.bmp",argv[3],i):snprintf(file,sizeof file,"%s",argv[3]);
            if(n<0 || (size_t)n>=sizeof file || rsx_sdl_gpu_backend_save_host_ui_bmp(file,atoi(argv[1]),atoi(argv[2]))) return 1;
        }
        return 0;
    }
    while(SDL_GetTicks()-start<(argc>5?strtoul(argv[5],NULL,10):60000)) {
        Uint64 now=SDL_GetTicks();
        if(!opening_pause) elapsed+=now-last;
        last=now;
        if(opening_restart || elapsed>3000) {
            elapsed=0;inside=0;opening_restart=0;
            taiko_preview_clock_ms=10000;opening_publish(0);
        }
        if(opening_step) {
            elapsed+=opening_step*1000.0/60;opening_step=0;
            if(elapsed<1000)elapsed=1000;
        }
        if(elapsed>=1000 && !inside) {
            taiko_preview_clock_ms=11000;opening_publish(1);inside=1;
        }
        taiko_preview_clock_ms=10000+elapsed;
        if(rsx_sdl_gpu_backend_main_iterate(16))break;
    }
    if(argc>3) return rsx_sdl_gpu_backend_save_host_ui_bmp(argv[3],atoi(argv[1]),atoi(argv[2]));
    return 0;
}
static int navigation_action;
static void navigation_key(int key)
{
    if(key==SDL_SCANCODE_LEFT || key==SDL_SCANCODE_D) navigation_action=-1;
    if(key==SDL_SCANCODE_RIGHT || key==SDL_SCANCODE_K) navigation_action=1;
    if(key==SDL_SCANCODE_RETURN) navigation_action=2;
    if(key==SDL_SCANCODE_ESCAPE) navigation_action=3;
    if(key==SDL_SCANCODE_HOME) navigation_action=4;
    if(key==SDL_SCANCODE_END) navigation_action=5;
    if(key==SDL_SCANCODE_M) navigation_action=6;
    if(key==SDL_SCANCODE_R) navigation_action=7;
}
static void navigation_publish(int inside,unsigned selected)
{
    if(!inside) { opening_publish(0);return; }
    static const char* names[]={"馬と鹿","Lemon","LOSER","刀剣乱舞","君はロックを聴かない",
        "マリーゴールド","ナンセンス文学","ロキ","YES or YES","ドラマツルギー"};
    const unsigned total=35;
    unsigned first=selected>5?selected-5:0;
    if(first+TAIKO_OVERLAY_SONG_ROW_COUNT>total) first=total-TAIKO_OVERLAY_SONG_ROW_COUNT;
    taiko_overlay_song_row rows[TAIKO_OVERLAY_SONG_ROW_COUNT]={0};
    for(unsigned i=0;i<TAIKO_OVERLAY_SONG_ROW_COUNT;++i) {
        unsigned pos=first+i;
        int back=pos==0 || pos==11 || pos==22 || pos==34;
        rows[i].title=back?"Return":names[(pos-1)%10];rows[i].genre="J-POP";
        rows[i].kind=back?TAIKO_OVERLAY_ROW_EXIT:TAIKO_OVERLAY_ROW_SONG;
        rows[i].catalog_index=pos;rows[i].browser_position=pos;rows[i].browser_total=total;
        rows[i].selected=pos==selected;
        rows[i].course_stars[0]=2;rows[i].course_stars[1]=4;rows[i].course_stars[2]=6;
    }
    const taiko_overlay_song_row* current=&rows[selected-first];
    taiko_overlay_show_song_browser("P1","",current->title,"J-POP",84,selected,31,881,"J-POP",0,12,"ONI",7,"",0,TAIKO_OVERLAY_BROWSER_SONGS,current->kind==TAIKO_OVERLAY_ROW_EXIT,rows,TAIKO_OVERLAY_SONG_ROW_COUNT);
}
static int navigation_preview(int argc,char** argv)
{
    Uint64 start=SDL_GetTicks(),cycle=start;
    int inside=0,stage=0,automatic=1;
    unsigned selected=0;
    int close_demo=!strncmp(argv[4],"back-",5);
    unsigned close_position=strstr(argv[4],"end")?34:strstr(argv[4],"middle")?22:0;
    taiko_overlay_set_browser_players(1,1,0,NULL);
    taiko_preview_clock_ms=10000;navigation_publish(0,0);
    if(strstr(argv[4],"-frame")) {
        int sequence=strstr(argv[4],"-frames")!=NULL;
        taiko_preview_clock_ms=11000;navigation_publish(1,0);
        taiko_preview_clock_ms=12000;navigation_publish(1,close_position);
        taiko_preview_clock_ms=13000;navigation_publish(0,0);
        unsigned count=sequence?(argc>5?strtoul(argv[5],NULL,10):54):1;
        double frame=argc>5?strtod(argv[5],NULL):48;
        for(unsigned i=0;i<count;++i) {
            char file[4096];
            taiko_preview_clock_ms=13000+(sequence?i:frame)*1000/60;
            int n=sequence?snprintf(file,sizeof file,"%s-%03u.bmp",argv[3],i):snprintf(file,sizeof file,"%s",argv[3]);
            if(n<0 || (size_t)n>=sizeof file || rsx_sdl_gpu_backend_save_host_ui_bmp(file,atoi(argv[1]),atoi(argv[2]))) return 1;
        }
        return 0;
    }
    g_rsx_replay_key_hook=navigation_key;
    fprintf(stderr,"Browser preview: Left/Right or D/K browse, Enter opens/returns, Escape backs out, Home/End jump, M middle Return, R restarts demo.\n");
    while(SDL_GetTicks()-start<(argc>5?strtoul(argv[5],NULL,10):120000)) {
        Uint64 now=SDL_GetTicks();
        double elapsed=now-cycle;
        taiko_preview_clock_ms=10000+now-start;
        int publish=0;
        if(navigation_action) {
            int action=navigation_action;navigation_action=0;automatic=0;
            if(action==7) { automatic=1;stage=0;cycle=now;inside=0;selected=0;publish=1; }
            else if(action==3) {inside=0;publish=1;}
            else if(action==2) {
                if(!inside) {inside=1;selected=0;publish=1;}
                else if(selected==0 || selected==11 || selected==22 || selected==34) {inside=0;publish=1;}
            } else if(inside) {
                selected=action==-1?(selected+34)%35:action==1?(selected+1)%35:action==4?0:action==5?34:22;
                publish=1;
            }
        }
        if(automatic) {
            if(elapsed>8000) {cycle=now;stage=0;inside=0;selected=0;publish=1;}
            else if(stage==0 && elapsed>=1000) {inside=1;selected=0;stage=1;publish=1;}
            else if(close_demo && stage==1 && elapsed>=2000) {selected=close_position;stage=2;publish=1;}
            else if(close_demo && stage==2 && elapsed>=4000) {inside=0;stage=3;publish=1;}
            else if(!close_demo && stage>=1 && stage<=10 && elapsed>=2000+(stage-1)*200) {selected=stage;stage++;publish=1;}
            else if(!close_demo && stage==11 && elapsed>=5600) {inside=0;stage++;publish=1;}
        }
        if(publish)navigation_publish(inside,selected);
        if(rsx_sdl_gpu_backend_main_iterate(16))break;
    }
    return rsx_sdl_gpu_backend_save_host_ui_bmp(argv[3],atoi(argv[1]),atoi(argv[2]));
}
static unsigned neighbour_category,neighbour_position,neighbour_open;
static const char* neighbour_folders[]={"J-POP","ANIME","VOCALOID","VARIETY","CLASSICAL","GAME MUSIC","NAMCO ORIGINAL","MEDLEY","CHILDREN'S SONGS","CUSTOM TJA","OSU! LAZER","NIJIIRO"};
static void neighbours_publish(void)
{
    static const char* songs[]={"馬と鹿","Lemon","LOSER","Silent Jealousy","紅蓮華","炎","1・2・3","Music","Next song","Another song"};
    unsigned category=neighbour_category,position=neighbour_position;
    taiko_overlay_song_row rows[TAIKO_OVERLAY_SONG_ROW_COUNT]={0};
    for(unsigned i=0;i<5;++i) {
        if(position)--position;
        else { category=(category+11)%12;position=(neighbour_open&(1u<<category))?34:0; }
    }
    for(unsigned i=0;i<TAIKO_OVERLAY_SONG_ROW_COUNT;++i) {
        int open=(neighbour_open&(1u<<category))!=0;
        int back=position==0 || position==11 || position==22 || position==34;
        rows[i].title=open?(back?"Return":songs[(position-1)%10]):neighbour_folders[category];
        rows[i].genre=neighbour_folders[category];rows[i].selected=i==5;
        rows[i].kind=open?(back?TAIKO_OVERLAY_ROW_EXIT:TAIKO_OVERLAY_ROW_SONG):TAIKO_OVERLAY_ROW_CATEGORY;
        rows[i].catalog_index=open?position:category==1?200:84;
        rows[i].browser_position=position;rows[i].browser_total=open?35:0;
        rows[i].carousel_group=category+1;
        rows[i].course_stars[0]=2;rows[i].course_stars[1]=4;rows[i].course_stars[2]=6;
        if(++position>=(open?35:1)) {position=0;category=(category+1)%12;}
    }
    int open=(neighbour_open&(1u<<neighbour_category))!=0;
    taiko_overlay_show_song_browser("P1","",rows[5].title,rows[5].genre,0,
        neighbour_position,34,881,open?neighbour_folders[neighbour_category]:"CATEGORIES",
        neighbour_category,12,"ONI",7,"",0,open?TAIKO_OVERLAY_BROWSER_SONGS:TAIKO_OVERLAY_BROWSER_CATEGORIES,
        rows[5].kind==TAIKO_OVERLAY_ROW_EXIT,rows,TAIKO_OVERLAY_SONG_ROW_COUNT);
}
static int neighbours_preview(int argc,char** argv)
{
    Uint64 start=SDL_GetTicks(),cycle=start;
    int stage=0,automatic=1;
    neighbour_category=neighbour_position=neighbour_open=0;
    taiko_overlay_set_browser_players(1,1,0,NULL);
    taiko_preview_clock_ms=10000;neighbours_publish();
    g_rsx_replay_key_hook=navigation_key;
    fprintf(stderr,"Shared carousel: arrows cross category boundaries; Enter opens/returns; Escape closes only this category; Home/End jump within list; R demo.\n");
    while(SDL_GetTicks()-start<(argc>5?strtoul(argv[5],NULL,10):180000)) {
        Uint64 now=SDL_GetTicks();double elapsed=now-cycle;
        taiko_preview_clock_ms=10000+now-start;
        int publish=0;
        if(navigation_action) {
            int action=navigation_action;navigation_action=0;automatic=0;
            int open=(neighbour_open&(1u<<neighbour_category))!=0;
            if(action==7) {automatic=1;cycle=now;stage=0;neighbour_category=neighbour_position=neighbour_open=0;publish=1;}
            else if(action==-1 || action==1) {
                int next=(int)neighbour_position+action;
                if(open && next>=0 && next<35)neighbour_position=next;
                else {
                    neighbour_category=(neighbour_category+12+action)%12;
                    neighbour_position=action<0 && (neighbour_open&(1u<<neighbour_category))?34:0;
                }
                publish=1;
            } else if(action==2 || action==3) {
                if(!open && action==2) {neighbour_open|=1u<<neighbour_category;neighbour_position=0;publish=1;}
                else if(open && (action==3 || neighbour_position==0 || neighbour_position==11 || neighbour_position==22 || neighbour_position==34)) {
                    neighbour_open&=~(1u<<neighbour_category);neighbour_position=0;publish=1;
                }
            } else if(open) {neighbour_position=action==4?0:action==5?34:22;publish=1;}
        }
        if(automatic) {
            if(elapsed>12000) {cycle=now;stage=0;neighbour_category=neighbour_position=neighbour_open=0;publish=1;}
            else if(stage==0 && elapsed>=1000) {neighbour_open=1;stage++;publish=1;}
            else if(stage==1 && elapsed>=2000) {neighbour_position=34;stage++;publish=1;}
            else if(stage==2 && elapsed>=3000) {neighbour_category=1;neighbour_position=0;stage++;publish=1;}
            else if(stage==3 && elapsed>=4500) {neighbour_open|=2;stage++;publish=1;}
            else if(stage==4 && elapsed>=6500) {neighbour_open&=~2u;stage++;publish=1;}
            else if(stage==5 && elapsed>=8500) {neighbour_category=0;neighbour_position=34;stage++;publish=1;}
            else if(stage==6 && elapsed>=10500) {neighbour_open=0;neighbour_position=0;stage++;publish=1;}
        }
        if(publish)neighbours_publish();
        if(rsx_sdl_gpu_backend_main_iterate(16))break;
    }
    return rsx_sdl_gpu_backend_save_host_ui_bmp(argv[3],atoi(argv[1]),atoi(argv[2]));
}
static int search_preview(int argc,char** argv)
{
    neighbour_category=1;neighbour_position=5;neighbour_open=2;
    taiko_overlay_set_browser_players(1,1,0,NULL);neighbours_publish();
    Uint64 start=SDL_GetTicks();unsigned stage=0;
    while(SDL_GetTicks()-start<4000) {
        unsigned elapsed=SDL_GetTicks()-start;
        unsigned next=elapsed<600?0:elapsed<1200?1:elapsed<2200?2:3;
        if(next!=stage) {
            stage=next;
            static const char* titles[]={"Dream", "Dreamers", "Dream Parade", "Dreaming", "夢の続き", "Dream Song", "Return"};
            static const char* genres[]={"J-POP","J-POP","ANIME","OSU! LAZER","VOCALOID","CUSTOM TJA","CUSTOM TJA"};
            taiko_overlay_song_row rows[7]={0};
            for(unsigned i=0;i<7;++i) {
                rows[i].title=titles[i];rows[i].genre=genres[i];rows[i].kind=i==6?TAIKO_OVERLAY_ROW_EXIT:TAIKO_OVERLAY_ROW_SONG;
                rows[i].catalog_index=i;rows[i].selected=i==2;rows[i].browser_position=i;rows[i].browser_total=7;
                rows[i].course_mask=15;for(unsigned d=0;d<4;++d)rows[i].course_stars[d]=d+3;
            }
            int empty=!strcmp(argv[4],"search-empty");
            int editing=strcmp(argv[4],"search-results") || stage<3;
            taiko_overlay_show_song_browser("P1","","Dream Parade","ANIME",0,3,empty?0:6,881,"SEARCH RESULTS",0,0,"ONI",15,
                stage==1?"":empty?"nothing matches":"dream",editing,TAIKO_OVERLAY_BROWSER_SONGS,0,empty?NULL:rows,empty?0:7);
        }
        if(rsx_sdl_gpu_backend_main_iterate(16))break;
    }
    return rsx_sdl_gpu_backend_save_host_ui_bmp(argv[3],atoi(argv[1]),atoi(argv[2]));
}

int main(int argc, char** argv)
{
    if (ps3_host_sdl_init(PS3_HOST_SDL_VIDEO | PS3_HOST_SDL_GAMEPAD) != 0 ||
        rsx_sdl_gpu_backend_main_init(1280,720,"Taiko browser GPU preview") != 0) return 1;
    SDL_Window* window = SDL_GetKeyboardFocus();
    if (!window) { int count; SDL_Window** windows=SDL_GetWindows(&count); if(count)window=windows[0]; SDL_free(windows); }
    if (window && argc > 2) SDL_SetWindowSize(window, atoi(argv[1]),atoi(argv[2]));
    if(argc>4 && !strncmp(argv[4],"search",6)) {
        int result=search_preview(argc,argv);
        unsigned errors=rsx_sdl_gpu_backend_error_count();
        rsx_sdl_gpu_backend_main_shutdown();ps3_host_sdl_shutdown();
        return result || errors;
    }
    if(argc>4 && (!strcmp(argv[4],"opening") || !strcmp(argv[4],"opening-frame") || !strcmp(argv[4],"opening-frames"))) {
        int result=opening_preview(argc,argv);
        unsigned errors=rsx_sdl_gpu_backend_error_count();
        rsx_sdl_gpu_backend_main_shutdown();ps3_host_sdl_shutdown();
        return result || errors;
    }
    if(argc>4 && (!strcmp(argv[4],"scrolling") || !strncmp(argv[4],"back-",5))) {
        int result=navigation_preview(argc,argv);
        unsigned errors=rsx_sdl_gpu_backend_error_count();
        rsx_sdl_gpu_backend_main_shutdown();ps3_host_sdl_shutdown();
        return result || errors;
    }
    if(argc>4 && !strcmp(argv[4],"neighbours")) {
        int result=neighbours_preview(argc,argv);
        unsigned errors=rsx_sdl_gpu_backend_error_count();
        rsx_sdl_gpu_backend_main_shutdown();ps3_host_sdl_shutdown();
        return result || errors;
    }
    const uint8_t difficulties[2]={3,2};
    taiko_overlay_set_browser_players(1,3,1,difficulties);
    taiko_overlay_song_row rows[TAIKO_OVERLAY_SONG_ROW_COUNT]={0};
    const char* titles[]={"Before the song","Another song","太鼓の達人 / Groove","EASY","NORMAL","HARD","ONI","URA","Next song"};
    for(unsigned i=0;i<9;++i){ rows[i].title=titles[i];rows[i].genre="VOCALOID";rows[i].catalog_index=i; }
    for(unsigned i=3;i<8;++i){rows[i].kind=TAIKO_OVERLAY_ROW_DIFFICULTY;rows[i].difficulty=i-3;rows[i].stars=i+1;}
    rows[2].selected=1; rows[5].cursors=2;rows[6].cursors=1;rows[6].ready=1;
    rows[5].selected=rows[6].selected=1;
    const int songs = argc > 4 && (!strcmp(argv[4], "songs") || !strcmp(argv[4], "return") || !strncmp(argv[4], "courses", 7));
    const int returning = songs && !strcmp(argv[4], "return");
    const int categories = argc > 4 && (!strcmp(argv[4], "categories") || !strcmp(argv[4], "anime"));
    const char* folders[]={"J-POP","ANIME","VOCALOID","VARIETY","CLASSICAL","GAME MUSIC","NAMCO ORIGINAL","MEDLEY","CHILDREN'S SONGS","CUSTOM TJA","OSU! LAZER","NIJIIRO"};
    if(categories) for(unsigned i=0;i<TAIKO_OVERLAY_SONG_ROW_COUNT;++i) {
        memset(&rows[i],0,sizeof rows[i]); rows[i].title=folders[i];rows[i].genre=folders[i];
        rows[i].kind=TAIKO_OVERLAY_ROW_CATEGORY;rows[i].catalog_index=47+i*51;rows[i].selected=i==5;
    }
    Uint64 start=SDL_GetTicks();
    unsigned last=~0u;
    while(SDL_GetTicks()-start<(argc>5 ? strtoul(argv[5],NULL,10) : 4000)){
        unsigned step=(unsigned)((SDL_GetTicks()-start)/200);
        if(step!=last){
            last=step; if(!categories && !songs) { rows[5].selected=step%2; rows[6].selected=!(step%2); }
            if(categories) {
                /* Move the same centred window as the production frontend. */
                unsigned selection=!strcmp(argv[4],"anime")?1:(12+8-(step/4)%12)%12;
                for(unsigned i=0;i<TAIKO_OVERLAY_SONG_ROW_COUNT;++i) {
                    unsigned category=(selection+12-5+i)%12;
                    rows[i].title=rows[i].genre=folders[category];
                    rows[i].catalog_index=47+category*51;
                    rows[i].selected=i==5;
                }
                taiko_overlay_show_song_browser("P1 + P2","",folders[selection],"CATEGORY FOLDER",149,6,12,9845,"CATEGORIES",6,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_CATEGORIES,0,rows,TAIKO_OVERLAY_SONG_ROW_COUNT);
            } else if(songs) {
                static const char* names[]={"Return","The New Adventure","1・2・3","88","1 Dream","太鼓の達人","Another song","Music","Next song"};
                const int course_preview=!strncmp(argv[4],"courses",7);
                for(unsigned i=0;i<9;++i) {
                    memset(&rows[i],0,sizeof rows[i]);rows[i].title=names[!course_preview && i==1?4:!course_preview && i==4?1:i];rows[i].genre="ANIME";
                    rows[i].catalog_index=i;rows[i].kind=i?TAIKO_OVERLAY_ROW_SONG:TAIKO_OVERLAY_ROW_EXIT;
                    rows[i].selected=i==(returning?0:course_preview?4:1);
                    rows[i].carousel_group=2;
                    rows[i].browser_position=i+(course_preview?20:0);
                    rows[i].browser_total=85;
                    if(!strcmp(argv[4],"courses-latin")) {
                        static const char* latin[]={"Return","ＤＲＥＡＭＥＲＳ","ＳＴＡＹ ＴＵＮＥ","ＴＴ -Japanese ver.-","ＤＲＥＡＭＥＲＳ","ＳＴＡＹ ＴＵＮＥ","ＴＴ","Music","Next song"};
                        rows[i].title=latin[i];
                    }
                    rows[i].course_mask=15;
                    rows[i].course_stars[0]=2;rows[i].course_stars[1]=3;rows[i].course_stars[2]=4;rows[i].course_stars[3]=8;
                    if(!strcmp(argv[4],"courses-five")) {rows[i].course_mask=31;rows[i].course_stars[4]=10;}
                    if(!strcmp(argv[4],"courses-one")) rows[i].course_mask=8;
                    if(!strcmp(argv[4],"courses-osu")) {
                        rows[i].chart_count=8;
                        for(unsigned d=0;d<5;++d)rows[i].chart_stars[d]=d+2;
                    }
                }
                taiko_overlay_show_song_browser("P1 + P2","preview",returning?"Return":"1 Dream","ANIME",123,1,85,881,"ANIME",1,12,"ONI",7,"",0,1,returning,rows,9);
            } else taiko_overlay_show_song_browser("P1 + P2","preview","太鼓の達人 / Groove","VOCALOID",123,12,47,881,"VOCALOID",2,9,"ONI",31,"",0,1,0,rows,9);
        }
        if(rsx_sdl_gpu_backend_main_iterate(16))break;
    }
    unsigned errors=rsx_sdl_gpu_backend_error_count();
    if (argc > 3) {
        /* Optional final argument: enter/leave captures the handoff midpoint. */
        if (argc > 4 && !categories && !songs) taiko_overlay_animate_browser(!strcmp(argv[4], "leave"));
        for (unsigned i = 0; i < 2; ++i) {
            if (i && argc > 4) SDL_Delay(160);
            if (rsx_sdl_gpu_backend_save_host_ui_bmp(argv[3], atoi(argv[1]), atoi(argv[2])) != 0) ++errors;
        }
    }
    rsx_sdl_gpu_backend_main_shutdown(); ps3_host_sdl_shutdown();
    return errors?1:0;
}
