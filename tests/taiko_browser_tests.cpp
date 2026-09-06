#include "taiko_frontend.h"
#include "taiko_catalog.h"
#include "taiko_host_audio.h"
#include "taiko_host_input.h"
#include "taiko_overlay.h"
#include "ppu_recomp.h"

#undef NDEBUG
#include <cassert>
#include <vector>

// Exercise the production frontend through its public keyboard/drum inputs.
// Only the guest, catalog I/O and drawing boundary are substituted.
static std::vector<TaikoCatalogSong> songs;
static std::vector<taiko_overlay_song_row> rows;
static unsigned current_song;
static int browser_level;
static uint8_t joined, ready;
static bool standalone = true;
static unsigned identity_requests;
uint32_t vm_read32(uint32_t) { return 0; }
uint8_t vm_read8(uint32_t) { return 0; }
void vm_write32(uint32_t, uint32_t) {}
extern "C" uint64_t ppu_guest_call_ct(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t) { return 0; }
extern "C" int taiko_pc_mode_is_active() { return 1; }
extern "C" int taiko_pc_mode_is_standalone() { return standalone; }
extern "C" void taiko_pc_mode_entry_tick(ppu_context*) {}
extern "C" int taiko_pc_mode_results_return(uint32_t, uint32_t) { return 0; }
void taiko_host_audio_select_preview(std::string_view, uint64_t) {}
static std::vector<TaikoPlusSfx> sounds;
void taiko_host_audio_play_sfx(TaikoPlusSfx s) { sounds.push_back(s); }
void taiko_host_audio_begin_gameplay_handoff() {}
void taiko_host_audio_reacquire_menu() {}
bool taiko_catalog_load() { return true; }
std::size_t taiko_catalog_count() { return songs.size(); }
const TaikoCatalogSong* taiko_catalog_song(std::size_t i) { return i < songs.size() ? &songs[i] : nullptr; }
const char* taiko_catalog_genre_name(const std::string& genre) { return genre.c_str(); }
const char* taiko_catalog_difficulty_name(unsigned d) {
    static const char* names[] = {"EASY", "NORMAL", "HARD", "ONI", "URA"};
    return names[d % 5];
}
bool taiko_catalog_content_identity(std::size_t, unsigned, taiko_plus::ContentIdentity&, std::string* error) {
    ++identity_requests;
    *error = "test stops before guest launch";
    return false;
}
extern "C" void taiko_overlay_show_song_browser(const char*, const char*, const char*, const char*,
    uint32_t, unsigned index, unsigned, unsigned, const char*, unsigned, unsigned,
    const char*, uint8_t, const char*, int, int level, int,
    const taiko_overlay_song_row* input, unsigned count) {
    rows.clear();
    if (count) rows.assign(input, input + count);
    current_song = index;
    browser_level = level;
    assert(count <= TAIKO_OVERLAY_SONG_ROW_COUNT);
}
extern "C" void taiko_overlay_set_browser_players(int, uint8_t j, uint8_t r, const uint8_t*) { joined = j; ready = r; }
extern "C" void taiko_overlay_show_song_select(const char*) {}
extern "C" void taiko_overlay_hide_host_screen() {}
extern "C" void taiko_overlay_clear() {}
extern "C" void taiko_overlay_show_entry_menu(int) {}
extern "C" void taiko_overlay_show_entry_progress(const char*) {}
extern "C" void taiko_overlay_show_baid_wait() {}

static unsigned courses() {
    unsigned count = 0;
    for (const auto& r : rows) count += r.kind == TAIKO_OVERLAY_ROW_DIFFICULTY;
    return count;
}
static unsigned cursor(unsigned player) {
    for (const auto& r : rows)
        if (r.kind == TAIKO_OVERLAY_ROW_DIFFICULTY && (r.cursors & (1u << player))) return r.difficulty;
    return 99;
}
static void key(unsigned command) { assert(taiko_frontend_browser_command(command)); }
static void drum(unsigned player, uint32_t action) {
    sounds.clear();
    assert(taiko_frontend_consume_press(player, action));
    assert(sounds.size() == 1); // Raw hit and navigation must not both play.
    assert(sounds[0] == ((action & (TAIKO_ACTION_HIT_SL | TAIKO_ACTION_HIT_SR))
                        ? TaikoPlusSfx::Move : TaikoPlusSfx::Confirm));
}

int main() {
    for (unsigned i = 0; i < 12; ++i) {
        TaikoCatalogSong song;
        song.music_id = "fixture" + std::to_string(i);
        song.title = (i < 10 ? "0" : "") + std::to_string(i);
        song.genre = "J-POP";
        song.difficulty_mask = i == 11 ? 0x04 : 0x1f;
        song.stars = {2, 4, 6, 8, 10};
        songs.push_back(song);
    }
    taiko_frontend_standalone_session_begin();
    taiko_frontend_enter_song_select_shell();
    assert(browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES && !courses());
    key(TAIKO_BROWSER_PLAY); // Category opens without launching or selecting a course.
    assert(browser_level == TAIKO_OVERLAY_BROWSER_SONGS && !courses());
    key(TAIKO_BROWSER_PLAY); // Keyboard opens and joins P1.
    assert(courses() == 5 && joined == 1 && cursor(0) == 3 && cursor(1) == 99);
    assert(identity_requests == 0 && !ready);
    for (const auto& r : rows)
        if (r.kind == TAIKO_OVERLAY_ROW_DIFFICULTY) assert(r.stars == songs[0].stars[r.difficulty]);
    drum(1, TAIKO_ACTION_HIT_SL); // P2 joins and moves only its own cursor.
    assert(joined == 3 && cursor(0) == 3 && cursor(1) == 2 && current_song == 0);
    drum(0, TAIKO_ACTION_HIT_CR);
    assert(ready == 1 && identity_requests == 0);
    drum(1, TAIKO_ACTION_HIT_SR);
    assert(cursor(0) == 3 && cursor(1) == 3 && ready == 1);
    drum(1, TAIKO_ACTION_HIT_CR); // Both ready: content resolution is attempted.
    assert(identity_requests == 1 && ready == 0 && courses() == 5);
    drum(1, TAIKO_ACTION_HIT_CL); // Either player can collapse without losing joins.
    assert(!courses() && joined == 3 && !ready);
    drum(1, TAIKO_ACTION_HIT_SR);
    assert(current_song == 1);
    drum(0, TAIKO_ACTION_HIT_SR);
    assert(current_song == 2);
    drum(1, TAIKO_ACTION_HIT_CR);
    assert(courses() == 5 && !ready);
    key(TAIKO_BROWSER_SEARCH_CLEAR);
    assert(!courses() && browser_level == TAIKO_OVERLAY_BROWSER_SONGS);
    key(TAIKO_BROWSER_LAST); // Last entry is the explicit exit card.
    key(TAIKO_BROWSER_PREVIOUS);
    assert(current_song == 11);
    key(TAIKO_BROWSER_PLAY);
    assert(courses() == 1 && cursor(0) == 2 && cursor(1) == 2);
    drum(0, TAIKO_ACTION_HIT_SL);
    drum(1, TAIKO_ACTION_HIT_SR);
    assert(cursor(0) == 2 && cursor(1) == 2 && current_song == 11);
    key(TAIKO_BROWSER_SEARCH_TOGGLE);
    assert(!courses());
    assert(taiko_frontend_browser_text("no match"));
    assert(rows.empty());
    key(TAIKO_BROWSER_PLAY);
    key(TAIKO_BROWSER_PLAY);
    assert(!courses() && identity_requests == 1); // Empty results cannot open/launch.
    key(TAIKO_BROWSER_SEARCH_CLEAR);
    key(TAIKO_BROWSER_FIRST);
    key(TAIKO_BROWSER_PLAY);
    assert(courses() == 5);
    key(TAIKO_BROWSER_SEARCH_CLEAR);
    key(TAIKO_BROWSER_SEARCH_CLEAR);
    assert(browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES && !courses());

    taiko_frontend_standalone_session_begin();
    taiko_frontend_enter_song_select_shell();
    drum(1, TAIKO_ACTION_HIT_CR);
    drum(1, TAIKO_ACTION_HIT_CR);
    assert(courses() == 5 && joined == 2 && cursor(0) == 99 && cursor(1) < 5);
    drum(1, TAIKO_ACTION_HIT_CR);
    assert(identity_requests == 2); // P2-only session does not wait for P1.

    standalone = false;
    taiko_frontend_standalone_session_begin();
    taiko_frontend_enter_song_select_shell();
    key(TAIKO_BROWSER_PLAY);
    key(TAIKO_BROWSER_PLAY);
    assert(courses() == 5 && cursor(0) < 5 && cursor(1) == 99);
    const auto before = cursor(0);
    key(TAIKO_BROWSER_NEXT);
    assert(cursor(0) != before);
    key(TAIKO_BROWSER_SEARCH_CLEAR);
    assert(!courses());
}
