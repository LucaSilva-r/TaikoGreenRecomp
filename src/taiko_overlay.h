/* On-screen overlay for the pairing code.
 *
 * The renderer draws the game's own font through FreeType, in the style the
 * title uses for its own text: white fill with a thick dark outline. The
 * result is a single RGBA image the SDL_GPU backend blits over the presented
 * frame, so no new pipeline or shader is involved.
 */
#ifndef TAIKO_OVERLAY_H
#define TAIKO_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Show `code` with a countdown; `expires_in` is seconds from now. */
void taiko_overlay_set_pairing(const char* code, int expires_in);
void taiko_overlay_clear(void);

/* The image to draw over the frame, or NULL when there is nothing to show.
 * `version` changes whenever the pixels do, so the backend can skip uploads.
 * The buffer belongs to the overlay and stays valid until the next call. */
const uint32_t* taiko_overlay_frame(int* width, int* height, uint32_t* version);

#ifdef __cplusplus
}
#endif

#endif /* TAIKO_OVERLAY_H */
