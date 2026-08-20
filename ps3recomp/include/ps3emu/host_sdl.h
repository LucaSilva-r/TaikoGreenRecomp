#ifndef PS3EMU_HOST_SDL_H
#define PS3EMU_HOST_SDL_H

#ifdef __cplusplus
extern "C" {
#endif

/* SDL subsystem ownership belongs to the executable's main thread. Backends
 * create and destroy only their own windows, devices, streams, and gamepads. */
enum {
    PS3_HOST_SDL_VIDEO = 1u << 0,
    PS3_HOST_SDL_GAMEPAD = 1u << 1,
    PS3_HOST_SDL_AUDIO = 1u << 2,
};
int ps3_host_sdl_init(unsigned requested_subsystems);
int ps3_host_sdl_audio_available(void);
void ps3_host_sdl_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
