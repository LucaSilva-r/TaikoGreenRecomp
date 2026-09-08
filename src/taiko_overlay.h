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
#include <stddef.h>

#include "rsx_host_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Show `code` with a countdown; `expires_in` is seconds from now. */
void taiko_overlay_set_pairing(const char* code, int expires_in);
/* Show a short appliance-status message using the same composited surface. */
void taiko_overlay_set_status(const char* text, int expires_in);
void taiko_overlay_set_browser_save_status(const char* text);
/* Opaque host-owned screens. These cover the guest frame completely so the
 * corresponding Lumen menu is neither visible nor used for interaction. */
void taiko_overlay_show_entry_menu(int selection);
void taiko_overlay_show_entry_progress(const char* player_name);
void taiko_overlay_show_baid_wait(void);
void taiko_overlay_show_song_select(const char* player_name);

enum { TAIKO_OVERLAY_SONG_ROW_COUNT = 9 };

enum taiko_overlay_browser_level {
    TAIKO_OVERLAY_BROWSER_CATEGORIES = 0,
    TAIKO_OVERLAY_BROWSER_SONGS = 1,
};

enum taiko_overlay_song_row_kind {
    TAIKO_OVERLAY_ROW_SONG = 0,
    TAIKO_OVERLAY_ROW_CATEGORY = 1,
    TAIKO_OVERLAY_ROW_EXIT = 2,
    TAIKO_OVERLAY_ROW_DIFFICULTY = 3,
};

typedef struct taiko_overlay_song_row {
    const char* title;
    const char* genre;
    unsigned catalog_index;
    int selected;
    int kind;
    unsigned difficulty; /* Chart identity within the song; independent of row position. */
    unsigned stars; /* Zero means the source has no rating. */
    uint8_t cursors;
    uint8_t ready;
} taiko_overlay_song_row;

void taiko_overlay_set_browser_players(int enabled, uint8_t joined, uint8_t ready,
                                      const uint8_t difficulties[2]);
void taiko_overlay_show_song_browser(const char* player_name,
                                     const char* music_id,
                                     const char* title,
                                     const char* genre,
                                     uint32_t unique_id,
                                     unsigned index,
                                     unsigned match_total,
                                     unsigned catalog_total,
                                     const char* category,
                                     unsigned category_index,
                                     unsigned category_total,
                                     const char* difficulty,
                                     uint8_t difficulty_mask,
                                     const char* query,
                                     int search_active,
                                     int browser_level,
                                     int selection_is_exit,
                                     const taiko_overlay_song_row* rows,
                                     unsigned row_count);
void taiko_overlay_hide_host_screen(void);
/* Animate the browser away over live gameplay (leaving=1), or bring its
 * panels in after Results (leaving=0). Does not delay guest scene lifetimes. */
void taiko_overlay_animate_browser(int leaving);
void taiko_overlay_clear(void);

/* Copy one coherent host-frame snapshot. Passing NULL as destination queries
 * metadata only. No mutable overlay storage is borrowed by the caller. */
int taiko_host_frame_copy(HostFrameInfo* info, void* destination,
                          size_t destination_bytes);

#ifdef __cplusplus
}
#endif

#endif /* TAIKO_OVERLAY_H */
