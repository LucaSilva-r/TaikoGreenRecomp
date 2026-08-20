#include <ps3emu/host_sdl.h>

#include <stdio.h>

#if defined(PS3RECOMP_RSX_BACKEND_SDL_GPU) || \
    defined(PS3RECOMP_INPUT_BACKEND_SDL3) || \
    defined(PS3RECOMP_AUDIO_BACKEND_SDL3)
#include <SDL3/SDL.h>

static int s_sdl_initialized;
static int s_sdl_audio_available;

int ps3_host_sdl_init(unsigned requested_subsystems)
{
    SDL_InitFlags flags = 0;
    if (requested_subsystems & PS3_HOST_SDL_VIDEO) flags |= SDL_INIT_VIDEO;
    if (requested_subsystems & PS3_HOST_SDL_GAMEPAD) flags |= SDL_INIT_GAMEPAD;
    if (requested_subsystems & PS3_HOST_SDL_AUDIO) flags |= SDL_INIT_AUDIO;
    if (!flags || s_sdl_initialized) return 0;

    if (SDL_Init(flags)) {
        s_sdl_initialized = 1;
        s_sdl_audio_available = (flags & SDL_INIT_AUDIO) != 0;
        return 0;
    }

    fprintf(stderr, "[host-sdl] SDL_Init(0x%X) failed: %s\n",
            (unsigned)flags, SDL_GetError());

#if defined(PS3RECOMP_AUDIO_BACKEND_SDL3)
    /* A missing audio endpoint must not take down a working video/input host.
     * Retry those required subsystems and let cellAudio select its explicit
     * null clock when it sees that audio is unavailable. */
    SDL_Quit();
    flags &= ~SDL_INIT_AUDIO;
    if (!flags) return 0;
    if (SDL_Init(flags)) {
        s_sdl_initialized = 1;
        fprintf(stderr, "[host-sdl] continuing without SDL audio\n");
        return 0;
    }
    fprintf(stderr, "[host-sdl] SDL retry without audio failed: %s\n",
            SDL_GetError());
#endif
    SDL_Quit();
    return -1;
}

int ps3_host_sdl_audio_available(void)
{
    return s_sdl_audio_available;
}

void ps3_host_sdl_shutdown(void)
{
    if (s_sdl_initialized) SDL_Quit();
    s_sdl_initialized = 0;
    s_sdl_audio_available = 0;
}

#else

int ps3_host_sdl_init(unsigned requested_subsystems)
{ (void)requested_subsystems; return 0; }
int ps3_host_sdl_audio_available(void) { return 0; }
void ps3_host_sdl_shutdown(void) {}

#endif
