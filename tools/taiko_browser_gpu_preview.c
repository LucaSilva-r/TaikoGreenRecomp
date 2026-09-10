/* Standalone host UI renderer smoke/preview; no guest or game dump required. */
#include "taiko_overlay.h"
#include "rsx_sdl_gpu_backend.h"
#include <ps3emu/host_sdl.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv)
{
    if (ps3_host_sdl_init(PS3_HOST_SDL_VIDEO | PS3_HOST_SDL_GAMEPAD) != 0 ||
        rsx_sdl_gpu_backend_main_init(1280,720,"Taiko browser GPU preview") != 0) return 1;
    SDL_Window* window = SDL_GetKeyboardFocus();
    if (!window) { int count; SDL_Window** windows=SDL_GetWindows(&count); if(count)window=windows[0]; SDL_free(windows); }
    if (window && argc > 2) SDL_SetWindowSize(window, atoi(argv[1]),atoi(argv[2]));
    const uint8_t difficulties[2]={3,2};
    taiko_overlay_set_browser_players(1,3,1,difficulties);
    taiko_overlay_song_row rows[9]={0};
    const char* titles[]={"Before the song","Another song","太鼓の達人 / Groove","EASY","NORMAL","HARD","ONI","URA","Next song"};
    for(unsigned i=0;i<9;++i){ rows[i].title=titles[i];rows[i].genre="VOCALOID";rows[i].catalog_index=i; }
    for(unsigned i=3;i<8;++i){rows[i].kind=TAIKO_OVERLAY_ROW_DIFFICULTY;rows[i].difficulty=i-3;rows[i].stars=i+1;}
    rows[2].selected=1; rows[5].cursors=2;rows[6].cursors=1;rows[6].ready=1;
    rows[5].selected=rows[6].selected=1;
    const int categories = argc > 4 && !strcmp(argv[4], "categories");
    const char* folders[]={"J-POP","ANIME","VOCALOID","VARIETY","CLASSICAL","GAME MUSIC","NAMCO ORIGINAL","MEDLEY","CHILDREN'S SONGS","CUSTOM TJA","OSU! LAZER","NIJIIRO"};
    if(categories) for(unsigned i=0;i<9;++i) {
        memset(&rows[i],0,sizeof rows[i]); rows[i].title=folders[i];rows[i].genre=folders[i];
        rows[i].kind=TAIKO_OVERLAY_ROW_CATEGORY;rows[i].catalog_index=47+i*51;rows[i].selected=i==4;
    }
    Uint64 start=SDL_GetTicks();
    unsigned last=~0u;
    while(SDL_GetTicks()-start<4000){
        unsigned step=(unsigned)((SDL_GetTicks()-start)/200);
        if(step!=last){
            last=step; if(!categories) { rows[5].selected=step%2; rows[6].selected=!(step%2); }
            if(categories) {
                /* Move the same centred window as the production frontend. */
                unsigned selection=step<5?8:step<10?7:6;
                for(unsigned i=0;i<9;++i) {
                    unsigned category=(selection+12-4+i)%12;
                    rows[i].title=rows[i].genre=folders[category];
                    rows[i].catalog_index=47+category*51;
                    rows[i].selected=i==4;
                }
                taiko_overlay_show_song_browser("P1 + P2","",folders[selection],"CATEGORY FOLDER",149,6,12,9845,"CATEGORIES",6,12,"",0,"",0,TAIKO_OVERLAY_BROWSER_CATEGORIES,0,rows,9);
            } else taiko_overlay_show_song_browser("P1 + P2","preview","太鼓の達人 / Groove","VOCALOID",123,12,47,881,"VOCALOID",2,9,"ONI",31,"",0,1,0,rows,9);
        }
        if(rsx_sdl_gpu_backend_main_iterate(16))break;
    }
    unsigned errors=rsx_sdl_gpu_backend_error_count();
    if (argc > 3) {
        /* Optional final argument: enter/leave captures the handoff midpoint. */
        if (argc > 4 && !categories) taiko_overlay_animate_browser(!strcmp(argv[4], "leave"));
        for (unsigned i = 0; i < 2; ++i) {
            if (i && argc > 4) SDL_Delay(160);
            if (rsx_sdl_gpu_backend_save_host_ui_bmp(argv[3], atoi(argv[1]), atoi(argv[2])) != 0) ++errors;
        }
    }
    rsx_sdl_gpu_backend_main_shutdown(); ps3_host_sdl_shutdown();
    return errors?1:0;
}
