#include "taiko_frontend.h"
#include "taiko_browser_accounts.h"
#include "taiko_catalog.h"
#include "taiko_host_audio.h"
#include "taiko_host_input.h"
#include "taiko_overlay.h"
#include "ppu_recomp.h"

#undef NDEBUG
#include <cassert>
#include <vector>
#include <unordered_map>
#include <cstring>

// Exercise the production frontend through its public keyboard/drum inputs.
// Only the guest, catalog I/O and drawing boundary are substituted.
static std::vector<TaikoCatalogSong> songs;
static std::vector<taiko_overlay_song_row> rows;
static unsigned current_song;
static int browser_level;
static int search_editing;
static std::string search_category,search_query;
static std::vector<std::string> row_sources;
static uint8_t joined, ready;
static bool standalone = true;
static unsigned identity_requests, identity_selection;
static std::unordered_map<uint32_t, uint8_t> memory;
uint8_t vm_read8(uint32_t a) { return memory[a]; }
uint32_t vm_read32(uint32_t a) {
    return uint32_t(vm_read8(a)) << 24 | uint32_t(vm_read8(a+1)) << 16 |
           uint32_t(vm_read8(a+2)) << 8 | vm_read8(a+3);
}
void vm_write8(uint32_t a, uint8_t v) { memory[a] = v; }
void vm_write32(uint32_t a, uint32_t v) {
    for (unsigned i=0; i<4; ++i) vm_write8(a+i, v >> (24-i*8));
}
static bool card_available, login_fixture;
static unsigned profile_commits;
static int login_phase;
extern "C" uint64_t taiko_card_browser_begin() { return 1; }
extern "C" void taiko_card_browser_end(uint64_t) {}
extern "C" int taiko_card_browser_take(uint64_t, char* code, uint8_t* uid) {
    if (!card_available) return 0;
    card_available = false;
    std::memcpy(code, "12345678901234567890", 21);
    std::memset(uid, 0, 4);
    return 1;
}
extern "C" void taiko_overlay_set_browser_login(int phase, const char*) { login_phase = phase; }
extern "C" uint64_t ppu_guest_call_ct(uint32_t fn, uint32_t toc, uint64_t a, uint64_t b, uint64_t, uint64_t) {
    if (!login_fixture) return 0;
    constexpr uint32_t receiver = 0xcffb1000, profile = 0x200008;
    if (fn == 0x00626e30 || fn == 0x006285b8) assert(toc == 0x01027c58);
    if (fn == 0x00233820) vm_write32(0x300008, a);
    if (fn == 0x00233804) vm_write32(0x300008, 0);
    if (fn == 0x00233254) {
        vm_write32(receiver+8, b); vm_write32(receiver+0xc, 0);
        vm_write32(receiver+0x1c, 0); vm_write8(receiver+1, 0);
    }
    if (fn == 0x000a1138 || fn == 0x000a0ca4 || fn == 0x000a0998) {
        const auto record = vm_read32(a);
        vm_write32(receiver+0xc, fn == 0x000a0998 ? 8 : fn == 0x000a0ca4 ? 7 : 3);
        vm_write32(receiver+0x1c, 1); vm_write8(receiver+1, 1);
        vm_write8(record+0x3ad, 1);
        return record;
    }
    if (fn == 0x005c59bc) return profile;
    if (fn == 0x006285b8 && a < 0xcffb0000) {
        assert(a == profile); // Accessor already skipped the key; no extra +8.
        ++profile_commits;
        vm_write32(profile+0x14, 4); vm_write32(profile+0x18, 15);
        for (unsigned i=0; i<4; ++i) vm_write8(profile+4+i, "Test"[i]);
    }
    return 0;
}
extern "C" int taiko_pc_mode_is_active() { return 1; }
extern "C" int taiko_pc_mode_is_standalone() { return standalone; }
extern "C" void taiko_pc_mode_entry_tick(ppu_context*) {}
extern "C" int taiko_pc_mode_results_return(uint32_t, uint32_t) { return 0; }
static unsigned preview_requests;
static std::string preview_id;
void taiko_host_audio_select_preview(std::string_view id, uint64_t) {
    ++preview_requests;
    preview_id = id;
}
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
bool taiko_catalog_content_identity(std::size_t selection, unsigned, taiko_plus::ContentIdentity&, std::string* error) {
    ++identity_requests;
    identity_selection = selection;
    *error = "test stops before guest launch";
    return false;
}
extern "C" void taiko_overlay_show_song_browser(const char*, const char*, const char*, const char*,
    uint32_t, unsigned index, unsigned, unsigned, const char* category, unsigned, unsigned,
    const char*, uint8_t, const char* query, int editing, int level, int,
    const taiko_overlay_song_row* input, unsigned count) {
    search_editing=editing;search_category=category?category:"";search_query=query?query:"";
    row_sources.clear();for(unsigned i=0;i<count;++i)row_sources.emplace_back(input[i].genre?input[i].genre:"");
    rows.clear();
    if (count) rows.assign(input, input + count);
    current_song = index;
    browser_level = level;
    assert(count <= TAIKO_OVERLAY_SONG_ROW_COUNT);
}
static taiko_overlay_difficulty_state difficulty_menu;
extern "C" void taiko_overlay_set_difficulty_menu(const taiko_overlay_difficulty_state* state) {difficulty_menu=*state;}
extern "C" void taiko_overlay_set_browser_players(int, uint8_t j, uint8_t r, const uint8_t*) { joined = j; ready = r; }
extern "C" void taiko_overlay_set_browser_account(unsigned, const char*, int) {}
extern "C" void taiko_overlay_show_song_select(const char*) {}
extern "C" void taiko_overlay_hide_host_screen() {}
extern "C" void taiko_overlay_animate_browser(int) {}
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

static void account_transactions() {
    using namespace taiko_plus;
    BrowserAccounts accounts;
    accounts.players[0] = {"Original", true};
    const auto cancelled = accounts.begin();
    assert(accounts.card_ready(cancelled));
    assert(!accounts.assign(2));
    assert(accounts.assign(0));
    assert(accounts.native_started(cancelled));
    accounts.cancel();
    assert(accounts.busy());
    assert(accounts.begin() == 0); // No receiver reuse while a callback can still arrive.
    assert(!accounts.native_finished(cancelled + 1));
    assert(accounts.native_finished(cancelled));
    assert(!accounts.busy());
    assert(!accounts.complete(cancelled, {"Late reply", true}));
    assert(accounts.players[0].name == "Original");
    const auto failed = accounts.begin();
    assert(accounts.card_ready(failed));
    assert(accounts.assign(0));
    accounts.fail(failed, "Network unavailable");
    assert(accounts.players[0].name == "Original");
    assert(!accounts.complete(failed, {"Late reply", true}));
    const auto success = accounts.begin();
    assert(!accounts.card_ready(failed));
    assert(accounts.card_ready(success));
    assert(accounts.assign(1));
    assert(!accounts.complete(success, {"Incomplete", false}));
    assert(accounts.complete(success, {"Second player", true}));
    accounts.fail(success, "Late error");
    assert(accounts.phase == AccountPhase::Idle);
    assert(accounts.players[0].name == "Original");
    assert(accounts.players[1].name == "Second player");
}

int main() {
    account_transactions();
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
    // The carousel stays centred while traversing both wrap boundaries.
    for (unsigned step=0; step<24; ++step) {
        assert(rows.size()==TAIKO_OVERLAY_SONG_ROW_COUNT && rows[rows.size()/2].selected);
        unsigned selected=0;
        for(const auto& row:rows) selected+=row.selected!=0;
        assert(selected==1 && current_song==step%12);
        key(TAIKO_BROWSER_NEXT);
    }
    key(TAIKO_BROWSER_PREVIOUS);
    assert(current_song==11 && rows[rows.size()/2].selected);
    key(TAIKO_BROWSER_NEXT);
    assert(current_song==0 && rows[rows.size()/2].selected);
    key(TAIKO_BROWSER_PLAY); // Category opens without launching or selecting a course.
    assert(browser_level == TAIKO_OVERLAY_BROWSER_SONGS && !courses());
    assert(rows[5].selected && rows[5].kind == TAIKO_OVERLAY_ROW_EXIT);
    for(unsigned i=5;i<rows.size();++i) {
        assert(rows[i].browser_position==i-5 && rows[i].browser_total==15);
    }
    key(TAIKO_BROWSER_PLAY); // Leading Return really closes the category.
    assert(browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES);
    key(TAIKO_BROWSER_PLAY);
    key(TAIKO_BROWSER_NEXT); // First song follows Return.
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
    assert(rows[5].browser_position==14 && rows[5].browser_total==15);
    key(TAIKO_BROWSER_PREVIOUS);
    assert(current_song == 11);
    key(TAIKO_BROWSER_PLAY);
    assert(courses() == 1 && cursor(0) == 2 && cursor(1) == 2);
    // Left reaches Sounds; right stops at the last installed course.
    drum(0, TAIKO_ACTION_HIT_SL);
    drum(1, TAIKO_ACTION_HIT_SR);
    assert(cursor(0) == 99 && cursor(1) == 2 && current_song == 11 &&
           difficulty_menu.item[0] == -1 && difficulty_menu.item[1] == 2);
    drum(1, TAIKO_ACTION_HIT_SL); // Sounds
    drum(1, TAIKO_ACTION_HIT_SL); // Options
    drum(1, TAIKO_ACTION_HIT_SL); // Back
    const auto before_edge_press=difficulty_menu.navigation_serial;
    drum(1, TAIKO_ACTION_HIT_SL); // Stop at Back, but still finish animation.
    assert(difficulty_menu.item[1] == -3);
    assert(difficulty_menu.navigation_serial==before_edge_press+1);
    drum(0, TAIKO_ACTION_HIT_CR); // Sounds opens the drum sound pane.
    assert(difficulty_menu.pane[0] == 2);
    drum(0, TAIKO_ACTION_HIT_SR); // Its single row cycles the value.
    assert(difficulty_menu.values[0][5] == 1);
    drum(0, TAIKO_ACTION_HIT_CL); // Cancel closes the pane, not the song.
    assert(difficulty_menu.pane[0] == 0 && courses() == 1);
    drum(1, TAIKO_ACTION_HIT_SR); // Back -> Options along the tab strip.
    assert(difficulty_menu.item[1] == -2 && !difficulty_menu.pane[1]);
    drum(1, TAIKO_ACTION_HIT_CR);
    assert(difficulty_menu.pane[1] == 1);
    drum(1, TAIKO_ACTION_HIT_SR); // The row cursor moves within the five settings.
    assert(difficulty_menu.option_row[1] == 1);
    drum(1, TAIKO_ACTION_HIT_CL);
    drum(1, TAIKO_ACTION_HIT_SL); // Options -> Back, then stop at the edge.
    drum(1, TAIKO_ACTION_HIT_SL);
    assert(difficulty_menu.item[1] == -3);
    drum(1, TAIKO_ACTION_HIT_SR); // Options
    drum(1, TAIKO_ACTION_HIT_SR); // Sounds
    drum(1, TAIKO_ACTION_HIT_SR); // Course
    drum(0, TAIKO_ACTION_HIT_SR);
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
    key(TAIKO_BROWSER_NEXT);
    key(TAIKO_BROWSER_PLAY);
    assert(courses() == 5);
    key(TAIKO_BROWSER_SEARCH_CLEAR);
    key(TAIKO_BROWSER_SEARCH_CLEAR);
    assert(browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES && !courses());

    taiko_frontend_standalone_session_begin();
    taiko_frontend_enter_song_select_shell();
    drum(1, TAIKO_ACTION_HIT_CR);
    drum(1, TAIKO_ACTION_HIT_SR);
    drum(1, TAIKO_ACTION_HIT_CR);
    assert(courses() == 5 && joined == 2 && cursor(0) == 99 && cursor(1) < 5);
    drum(1, TAIKO_ACTION_HIT_CR);
    assert(identity_requests == 2); // P2-only session does not wait for P1.

    // Lineup changes are possible without navigating away from the browser.
    taiko_frontend_standalone_session_begin();
    taiko_frontend_enter_song_select_shell();
    key(TAIKO_BROWSER_PLAYER1_TOGGLE);
    key(TAIKO_BROWSER_PLAYER2_TOGGLE);
    assert(joined == 3);
    key(TAIKO_BROWSER_PLAY);
    key(TAIKO_BROWSER_NEXT);
    key(TAIKO_BROWSER_PLAY);
    drum(0, TAIKO_ACTION_HIT_CR);
    assert(ready == 1);
    const unsigned launches_before_leave = identity_requests;
    key(TAIKO_BROWSER_PLAYER2_TOGGLE);
    assert(joined == 1 && ready == 0 && identity_requests == launches_before_leave);
    key(TAIKO_BROWSER_PLAYER1_TOGGLE);
    assert(joined == 0 && !courses());
    key(TAIKO_BROWSER_PLAYER2_TOGGLE);
    assert(joined == 2 && ready == 0);

    taiko_frontend_standalone_session_begin();
    taiko_frontend_enter_song_select_shell();
    login_fixture = card_available = true;
    vm_write32(0x01033f08, 0x300000);
    vm_write32(0x100374, 1); vm_write32(0x100370, 0x200000);
    key(TAIKO_BROWSER_ACCOUNT_LOGIN);
    taiko_frontend_browser_login_tick(0x100000, 0);
    key(TAIKO_BROWSER_PLAYER1_TOGGLE); // Assign the card to P1.
    for (unsigned i=0; i<4; ++i) taiko_frontend_browser_login_tick(0x100000, 0);
    assert(profile_commits == 1 && joined == 1);
    assert(login_phase == int(taiko_plus::AccountPhase::Idle));
    assert(vm_read32(0x300008) == 0); // Release the native response receiver.
    login_fixture = false;

    // Custom folders remain under the single top-level CUSTOM TJA category.
    TaikoCatalogSong custom;
    custom.music_id = "tc_fixture";
    custom.title = "Folder song";
    custom.genre = "CUSTOM TJA";
    custom.custom_folder = "Anime";
    custom.difficulty_mask = 8;
    songs.push_back(custom);
    custom.music_id = "tc_fixture2"; custom.title = "Second folder song";
    songs.push_back(custom);
    taiko_frontend_standalone_session_begin();
    taiko_frontend_enter_song_select_shell();
    key(TAIKO_BROWSER_LAST);
    key(TAIKO_BROWSER_PREVIOUS);
    key(TAIKO_BROWSER_PREVIOUS); // CUSTOM TJA precedes OSU! LAZER and NIJIIRO.
    key(TAIKO_BROWSER_PLAY);
    assert(rows.size() == 2 && rows[0].kind == TAIKO_OVERLAY_ROW_CATEGORY);
    assert(rows[0].catalog_index == 2);
    key(TAIKO_BROWSER_PLAY); // Anime
    assert(rows.size() == 3 && rows[0].kind == TAIKO_OVERLAY_ROW_SONG);
    key(TAIKO_BROWSER_PLAY);
    assert(courses() == 1);
    key(TAIKO_BROWSER_SEARCH_CLEAR); // collapse
    key(TAIKO_BROWSER_SEARCH_CLEAR); // parent
    assert(rows[0].kind == TAIKO_OVERLAY_ROW_CATEGORY);
    assert(browser_level == TAIKO_OVERLAY_BROWSER_SONGS);
    key(TAIKO_BROWSER_SEARCH_CLEAR); // stock categories
    assert(browser_level == TAIKO_OVERLAY_BROWSER_CATEGORIES);
    key(TAIKO_BROWSER_FIRST);

    // Named osu charts group by set, not by title, and scroll beyond five.
    const unsigned osu_begin = songs.size();
    for (unsigned i = 0; i < 12; ++i) {
        TaikoCatalogSong chart;
        chart.music_id = "osu" + std::to_string(i);
        chart.title = "Grouped song";
        chart.genre = "OSU! LAZER";
        chart.osu_group = "set-a";
        chart.osu_difficulty = "Named " + std::to_string(i);
        chart.stars[3] = 1 + i / 2;
        chart.difficulty_mask = 8;
        songs.push_back(chart);
    }
    taiko_frontend_standalone_session_begin();
    taiko_frontend_enter_song_select_shell();
    key(TAIKO_BROWSER_LAST);
    key(TAIKO_BROWSER_PREVIOUS); // OSU! LAZER
    key(TAIKO_BROWSER_PLAY);
    assert(rows.size() == TAIKO_OVERLAY_SONG_ROW_COUNT && rows[5].kind == TAIKO_OVERLAY_ROW_EXIT && rows[5].selected);
    key(TAIKO_BROWSER_NEXT);
    assert(rows[5].chart_count == 12 && rows[5].course_mask == 8);
    assert(rows[5].chart_stars[0] == 1 && rows[5].chart_stars[4] == 3);
    key(TAIKO_BROWSER_PLAY);
    assert(courses() == 8); // Visible window, not a five-course truncation.
    const unsigned preview_before = preview_requests;
    const std::string original_preview = preview_id;
    for (unsigned i = 0; i < 10; ++i) key(TAIKO_BROWSER_NEXT);
    bool reached_tail = false;
    for (const auto& row : rows)
        if (row.kind == TAIKO_OVERLAY_ROW_DIFFICULTY && row.selected)
            reached_tail = row.catalog_index >= osu_begin + 10;
    assert(reached_tail);
    for(const auto& row:rows) if(row.kind==TAIKO_OVERLAY_ROW_DIFFICULTY) {
        assert(row.browser_total==12);
        assert(row.browser_position==row.catalog_index-osu_begin);
    }
    assert(preview_requests == preview_before && preview_id == original_preview);
    key(TAIKO_BROWSER_PLAY);
    assert(identity_selection == osu_begin + 10);
    key(TAIKO_BROWSER_SEARCH_CLEAR);
    key(TAIKO_BROWSER_SEARCH_CLEAR);
    key(TAIKO_BROWSER_FIRST);

    songs[osu_begin].title = "Kawaki wo Ameku";
    songs[osu_begin].original_title = "カワキヲアメク";
    taiko_frontend_standalone_session_begin();
    taiko_frontend_enter_song_select_shell();
    for (const char* query : {"kawa", "KAWAKI WO AMEKU", "カワキヲアメク"}) {
        key(TAIKO_BROWSER_SEARCH_TOGGLE);
        assert(taiko_frontend_browser_text(query));
        assert(!rows.empty() && rows[0].kind == TAIKO_OVERLAY_ROW_SONG);
        assert(rows.size() == 2); // One matching set and its exit row.
        key(TAIKO_BROWSER_SEARCH_CLEAR);
    }

    // Global search from inside a category keeps source colours, opens a
    // result collection on the first Enter, and restores the list on Escape.
    const auto stock_title=songs[0].title,osu_title=songs[osu_begin].title;
    songs[0].title=songs[osu_begin].title="Shared Song";
    key(TAIKO_BROWSER_FIRST);key(TAIKO_BROWSER_PLAY);key(TAIKO_BROWSER_NEXT);
    key(TAIKO_BROWSER_SEARCH_TOGGLE);
    assert(search_editing && taiko_frontend_browser_captures_text());
    assert(taiko_frontend_browser_text("Shared Song猫"));
    assert(rows.empty());
    key(TAIKO_BROWSER_SEARCH_BACKSPACE);
    assert(search_query=="Shared Song" && rows.size()==3);
    bool stock_source=false,osu_source=false;
    for(const auto& source:row_sources) {stock_source|=source=="J-POP";osu_source|=source=="OSU! LAZER";}
    assert(stock_source && osu_source);
    const auto launches=identity_requests;
    key(TAIKO_BROWSER_PLAY);
    assert(!search_editing && !taiko_frontend_browser_captures_text());
    assert(search_category=="SEARCH RESULTS" && !courses() && identity_requests==launches);
    key(TAIKO_BROWSER_PLAY);assert(courses()==5);
    key(TAIKO_BROWSER_SEARCH_CLEAR);key(TAIKO_BROWSER_SEARCH_CLEAR);
    assert(search_category=="J-POP" && browser_level==TAIKO_OVERLAY_BROWSER_SONGS && search_query.empty());
    songs[0].title=stock_title;songs[osu_begin].title=osu_title;

    standalone = false;
    taiko_frontend_standalone_session_begin();
    taiko_frontend_enter_song_select_shell();
    key(TAIKO_BROWSER_PLAY);
    key(TAIKO_BROWSER_NEXT);
    key(TAIKO_BROWSER_PLAY);
    assert(courses() == 5 && cursor(0) < 5 && cursor(1) == 99);
    const auto before = cursor(0);
    key(TAIKO_BROWSER_NEXT);
    assert(cursor(0) != before);
    key(TAIKO_BROWSER_SEARCH_CLEAR);
    assert(!courses());
    // Open lists remain in the shared carousel when their boundary is crossed.
    standalone=true;
    for(unsigned i=0;i<2;++i) {
        TaikoCatalogSong song;song.music_id="anime"+std::to_string(i);
        song.title="Anime "+std::to_string(i);song.genre="アニメ";song.difficulty_mask=8;
        songs.push_back(song);
    }
    taiko_frontend_standalone_session_begin();taiko_frontend_enter_song_select_shell();
    key(TAIKO_BROWSER_FIRST);key(TAIKO_BROWSER_PLAY);key(TAIKO_BROWSER_LAST);
    key(TAIKO_BROWSER_NEXT); // J-POP end -> unopened Anime, not J-POP start.
    assert(browser_level==TAIKO_OVERLAY_BROWSER_CATEGORIES && rows[5].carousel_group==2);
    assert(rows[4].carousel_group==1 && rows[4].kind==TAIKO_OVERLAY_ROW_EXIT && rows[4].browser_total==15);
    key(TAIKO_BROWSER_PLAY); // Both categories are now open.
    assert(browser_level==TAIKO_OVERLAY_BROWSER_SONGS && rows[5].carousel_group==2);
    assert(rows[4].carousel_group==1 && rows[4].browser_total==15);
    key(TAIKO_BROWSER_LAST);key(TAIKO_BROWSER_NEXT); // Anime end -> Vocaloid.
    assert(browser_level==TAIKO_OVERLAY_BROWSER_CATEGORIES && rows[5].carousel_group==3);
    key(TAIKO_BROWSER_PREVIOUS); // Re-enter Anime at its still-open last Return.
    assert(browser_level==TAIKO_OVERLAY_BROWSER_SONGS && rows[5].browser_position==3);
    key(TAIKO_BROWSER_PLAY); // Close only Anime.
    assert(browser_level==TAIKO_OVERLAY_BROWSER_CATEGORIES && rows[5].carousel_group==2);
    assert(rows[4].carousel_group==1 && rows[4].browser_total==15);
    key(TAIKO_BROWSER_PREVIOUS); // J-POP is still open.
    assert(browser_level==TAIKO_OVERLAY_BROWSER_SONGS && rows[5].browser_position==14);
    key(TAIKO_BROWSER_FIRST);key(TAIKO_BROWSER_PREVIOUS); // Beginning -> previous category.
    assert(browser_level==TAIKO_OVERLAY_BROWSER_CATEGORIES && rows[5].carousel_group==12);
    key(TAIKO_BROWSER_NEXT);
    assert(browser_level==TAIKO_OVERLAY_BROWSER_SONGS && rows[5].carousel_group==1 && rows[5].browser_position==0);

}
extern "C" void taiko_overlay_set_browser_save_status(const char*) {}
