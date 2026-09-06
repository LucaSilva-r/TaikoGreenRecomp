/* Standalone host UI renderer smoke/preview; no guest or game dump required. */
#include "taiko_overlay.h"
#include "rsx_sdl_gpu_backend.h"
#include <ps3emu/host_sdl.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
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
    Uint64 start=SDL_GetTicks();
    unsigned last=~0u;
    while(SDL_GetTicks()-start<4000){
        unsigned step=(unsigned)((SDL_GetTicks()-start)/200);
        if(step!=last){
            last=step; rows[5].selected=step%2; rows[6].selected=!(step%2);
            taiko_overlay_show_song_browser("P1 + P2","preview","太鼓の達人 / Groove","VOCALOID",123,12,47,881,"VOCALOID",2,9,"ONI",31,"",0,1,0,rows,9);
        }
        if(rsx_sdl_gpu_backend_main_iterate(16))break;
    }
    unsigned errors=rsx_sdl_gpu_backend_error_count();
    if (argc > 3) {
        for (unsigned i = 0; i < 2; ++i)
            if (rsx_sdl_gpu_backend_save_host_ui_bmp(argv[3], atoi(argv[1]), atoi(argv[2])) != 0) ++errors;
    }
    rsx_sdl_gpu_backend_main_shutdown(); ps3_host_sdl_shutdown();
    return errors?1:0;
}
