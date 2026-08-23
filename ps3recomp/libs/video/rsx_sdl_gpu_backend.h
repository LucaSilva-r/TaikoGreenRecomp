#ifndef PS3RECOMP_RSX_SDL_GPU_BACKEND_H
#define PS3RECOMP_RSX_SDL_GPU_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

struct rsx_render_batch;

/* SDL window, event, and GPU ownership is deliberately main-thread-only. */
int rsx_sdl_gpu_backend_main_init(unsigned width, unsigned height,
                                  const char* title);
int rsx_sdl_gpu_backend_main_iterate(int timeout_ms);
void rsx_sdl_gpu_backend_main_shutdown(void);

/* Called by the FIFO producer to avoid entering a recorder flush while all
 * four immutable queue slots are occupied. */
int rsx_sdl_gpu_backend_queue_has_capacity(void);
int rsx_sdl_gpu_backend_has_pending_batches(void);
unsigned rsx_sdl_gpu_backend_error_count(void);
int rsx_sdl_gpu_backend_submit_batch(const struct rsx_render_batch* batch);
int rsx_sdl_gpu_backend_save_display_bmp(const char* path);

#ifdef RSX_SDL_REPLAY_STANDALONE
/* Replay-only key routing: the backend owns the SDL event pump, so rsx_replay's
 * frame-marking mode needs the keys handed to it. */
extern void (*g_rsx_replay_key_hook)(int scancode);
#endif

#ifdef __cplusplus
}
#endif
#endif
