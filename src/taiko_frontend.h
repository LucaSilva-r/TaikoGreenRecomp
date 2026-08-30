#ifndef TAIKO_FRONTEND_H
#define TAIKO_FRONTEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ppu_context;

#if defined(__GNUC__)
#define TAIKO_FRONTEND_OPTIONAL __attribute__((weak))
#else
#define TAIKO_FRONTEND_OPTIONAL
#endif

/* Input-side filters. A nonzero return from press/release means the host
 * frontend owns the action and it must not be forwarded to USIO. */
int taiko_frontend_consume_press(unsigned player, uint32_t actions)
    TAIKO_FRONTEND_OPTIONAL;
int taiko_frontend_consume_release(unsigned player, uint32_t actions)
    TAIKO_FRONTEND_OPTIONAL;
uint32_t taiko_frontend_filter_levels(unsigned player, uint32_t levels,
                                      uint64_t timestamp_ns)
    TAIKO_FRONTEND_OPTIONAL;

/* Desktop/kiosk browser controls which do not map cleanly to cabinet USIO.
 * These remain host-only and are accepted only while the Song Select shell
 * owns the screen. */
enum taiko_frontend_browser_command {
    TAIKO_BROWSER_SEARCH_TOGGLE = 1,
    TAIKO_BROWSER_SEARCH_CLEAR,
    TAIKO_BROWSER_SEARCH_BACKSPACE,
    TAIKO_BROWSER_PREVIOUS,
    TAIKO_BROWSER_NEXT,
    TAIKO_BROWSER_PREVIOUS_PAGE,
    TAIKO_BROWSER_NEXT_PAGE,
    TAIKO_BROWSER_FIRST,
    TAIKO_BROWSER_LAST,
    TAIKO_BROWSER_RANDOM,
    TAIKO_BROWSER_CATEGORY_PREVIOUS,
    TAIKO_BROWSER_CATEGORY_NEXT,
    TAIKO_BROWSER_DIFFICULTY_PREVIOUS,
    TAIKO_BROWSER_DIFFICULTY_NEXT,
    TAIKO_BROWSER_PLAY,
};

int taiko_frontend_browser_command(unsigned command)
    TAIKO_FRONTEND_OPTIONAL;
int taiko_frontend_browser_text(const char* text)
    TAIKO_FRONTEND_OPTIONAL;
int taiko_frontend_browser_captures_text(void)
    TAIKO_FRONTEND_OPTIONAL;

/* Called at the verified Player Entry dispatcher boundary on the main PPU
 * thread. Host/UI threads only enqueue intent; all guest calls happen here. */
void taiko_frontend_guest_tick(struct ppu_context* ctx);

/* Called from the normal GameSongSelect state-machine boundary.  It performs
 * a requested host-browser selection through Green's native music manager. */
void taiko_frontend_song_select_tick(struct ppu_context* ctx);

/* Called from normal GameEnsoResult's final-session destination. Returns
 * nonzero after substituting Green's native "another song" transition. */
int taiko_frontend_results_end_override(struct ppu_context* ctx);

/* Called whenever Results constructs Green's next normal Song Select. */
void taiko_frontend_results_continue_tick(struct ppu_context* ctx);

#undef TAIKO_FRONTEND_OPTIONAL

#ifdef __cplusplus
}
#endif

#endif
