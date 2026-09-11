#include <future>
/* Host-owned Player Entry frontend.
 *
 * The SDL/input side only updates atomics. The guest side runs from the start
 * of Green's Player Entry dispatcher (func_001EF214), which is the verified
 * main-thread safe point for this scene. Anonymous invokes Green's complete
 * no-card join callback, func_00226A9C, with its two native integer arguments;
 * BAID leaves the existing virtual reader, baidcheck, userdata and costume
 * managers intact. Both paths then invoke Green's native game-mode callback,
 * func_002287BC, and enter its stock WaitEndInterval handoff.
 */
#define TAIKO_FRONTEND_IMPLEMENTATION
#include "taiko_frontend.h"
#undef TAIKO_FRONTEND_IMPLEMENTATION
#include "taiko_pc_mode.h"

#include "ppu_recomp.h"
#include "taiko_catalog.h"
#include "taiko_host_audio.h"
#include "taiko_host_input.h"
#include "taiko_overlay.h"
#include "taiko_plus_runtime.h"
#include "taiko_browser_players.h"
#include "taiko_browser_accounts.h"
#include "taiko_card.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>

extern "C" uint64_t ppu_guest_call_ct(uint32_t code, uint32_t toc,
                                        uint64_t a0, uint64_t a1,
                                        uint64_t a2, uint64_t a3);

namespace {

thread_local bool g_drum_feedback = false;
void browser_sfx(TaikoPlusSfx sound)
{
    if (!g_drum_feedback) taiko_host_audio_play_sfx(sound);
}

constexpr uint32_t kPlayerRecordOffset = 0x38;
constexpr uint32_t kPlayerRecordSize = 0x4F0;
constexpr uint32_t kEntrySubcontrollerOffset = 0xF60;
constexpr uint32_t kUserDataControllerOffset = 0x1048;
constexpr uint32_t kOnlineCompletionFlagOffset = 0x1188;
constexpr uint32_t kStateOffset = 0x14;
constexpr uint32_t kIntervalOffset = 0x18;

constexpr uint32_t kStateEntryMain = 6;
constexpr uint32_t kStateCardSelect = 7;
constexpr uint32_t kStateUserDataWait = 23;
constexpr uint32_t kStateWaitEndInterval = 35;
constexpr uint32_t kStateEnd = 39;
constexpr uint32_t kStateTerm = 40;

constexpr uint32_t kAuthenticatedProfileCopy = 0x00225CB8;
constexpr uint32_t kAnonymousJoin = 0x00226A9C;
constexpr uint32_t kGameModeCommit = 0x002287BC;
constexpr uint32_t kEntrySetState = 0x001ED698;
constexpr uint32_t kEntrySubcontrollerReset = 0x002292F8;
constexpr uint32_t kUserDataBegin = 0x002332C4;

/* Normal cabinet GameSongSelect (not Ghost/Waiwai Song Select). */
constexpr uint32_t kGameSongSelectVtable = 0x00F92FC0;
constexpr uint32_t kSongSelectManagerOffset = 0x0C;
constexpr uint32_t kSongSelectStateOffset = 0x10;
constexpr uint32_t kSongSelectNextOffset = 0x14;
constexpr uint32_t kSongSelectP1DifficultyOffset = 0x20;
constexpr uint32_t kSongSelectP2DifficultyOffset = 0x50;
constexpr uint32_t kSongSelectCallbackOffset = 0xC8;
constexpr uint32_t kSongSelectModeFlagOffset = 0xF68;
constexpr uint32_t kSongVectorBeginOffset = 0x434;
constexpr uint32_t kSongVectorEndOffset = 0x438;
constexpr uint32_t kSongRecordSize = 0x90;
constexpr uint32_t kSongRecordMusicIdOffset = 0x04;
constexpr uint32_t kSongSelectCallback = 0x002456B0;
constexpr uint32_t kMusicSelectionCommit = 0x007FCE6C;

/* Normal cabinet GameEnsoResult and its stock "another song" transition. */
constexpr uint32_t kGameEnsoResultVtable = 0x00F92C98;
constexpr uint32_t kResultsContinueToSongSelect = 0x001EBEB0;

/* Private guest scratch, below ppu_guest_call_ct's 0xCFFE0000 stack top. */
constexpr uint32_t kCallbackFrame = 0xCFFD0000;
constexpr uint32_t kCallbackStack = kCallbackFrame + 0x40;
constexpr uint32_t kCallbackArgument1 = 0xCFFD1000;
constexpr uint32_t kCallbackArgument2 = kCallbackArgument1 - 8;
constexpr uint32_t kCallbackArgument3 = kCallbackArgument1 - 16;
constexpr uint32_t kCallbackBufferEnd = kCallbackArgument1 + 0x100;

enum class Phase : uint32_t {
    WaitingForEntry,
    LoginMenu,
    AnonymousRequested,
    BaidWaiting,
    Finishing,
    SongSelect,
    Passthrough,
    Failed,
};

std::atomic<Phase> g_phase{Phase::WaitingForEntry};
std::atomic<unsigned> g_selection{0};
std::atomic<unsigned> g_song_selection{0};
std::atomic<unsigned> g_song_difficulty{TAIKO_DIFFICULTY_ONI};
std::recursive_mutex g_browser_action_lock;
taiko_plus::BrowserPlayers g_browser_players;
taiko_plus::BrowserAccounts g_browser_accounts;
uint64_t g_browser_card_lease = 0;
char g_browser_card_code[21] = {};
uint8_t g_browser_card_uid[4] = {};
void show_browser_login() {
    taiko_overlay_set_browser_login(static_cast<int>(g_browser_accounts.phase),
                                     g_browser_accounts.status.c_str());
}
void cancel_browser_login() {
    taiko_card_browser_end(g_browser_card_lease);
    g_browser_card_lease = 0;
    std::memset(g_browser_card_code, 0, sizeof g_browser_card_code);
    g_browser_accounts.cancel();
    show_browser_login();
}

unsigned g_player_song_index = ~0u;
std::atomic<bool> g_song_launch_requested{false};
std::atomic<bool> g_song_search_active{false};
std::mutex g_song_browser_lock;
std::vector<unsigned> g_song_matches;
struct SongBrowserEntry {
    bool exit_category;
    unsigned catalog_index;
    unsigned song_position;
    std::string folder;
};
std::vector<SongBrowserEntry> g_song_entries;
std::array<std::vector<SongBrowserEntry>,12> g_open_category_entries;
std::array<bool,12> g_open_categories{};
uint64_t g_song_entries_generation;
std::array<uint64_t,12> g_open_category_generation{};
std::string g_song_query;
unsigned g_song_browser_position = 0;
unsigned g_song_category = 0;
std::string g_custom_folder;
std::unordered_map<std::string, std::vector<unsigned>> g_osu_groups;
unsigned g_osu_variant = 0;
enum class SongBrowserLevel { Categories, Songs };
SongBrowserLevel g_song_browser_level = SongBrowserLevel::Categories;
bool g_song_global_search = false;
SongBrowserLevel g_search_return_level=SongBrowserLevel::Categories;
unsigned g_search_return_position=0, g_search_return_selection=0;
std::atomic<uint32_t> g_random_state{0x6D2B79F5u};
std::atomic<uint64_t> g_preview_generation{0};
std::mutex g_preview_lock;
std::string g_preview_music_id;
std::array<std::atomic<uint32_t>, 2> g_raw_levels{};
uint32_t g_controller = 0;
uint32_t g_toc = 0;
unsigned g_finish_delay = 0;
unsigned g_finish_stage = 0;
unsigned g_finish_timeout = 0;
unsigned g_card_select_ticks = 0;
const char* g_session_label = "P1";

struct SongCategory {
    const char* label;
    const char* genre;
};

constexpr std::array<SongCategory, 12> kSongCategories{{
    {"J-POP", "J-POP"},
    {"ANIME", "アニメ"},
    {"VOCALOID", "ボーカロイド"},
    {"VARIETY", "バラエティ"},
    {"CLASSICAL", "クラシック"},
    {"GAME MUSIC", "ゲームミュージック"},
    {"NAMCO ORIGINAL", "ナムコオリジナル"},
    {"MEDLEY", "メドレー"},
    {"CHILDREN'S SONGS", "童謡"},
    {"CUSTOM TJA", "CUSTOM TJA"},
    {"OSU! LAZER", "OSU! LAZER"},
    {"NIJIIRO", "NIJIIRO"},
}};

bool enabled()
{
    static const bool value = [] {
        const char* setting = std::getenv("TAIKO_HOST_FRONTEND");
        return setting && setting[0] && std::strcmp(setting, "0") != 0;
    }();
    return value;
}

bool frontend_owns_input()
{
    const Phase phase = g_phase.load(std::memory_order_acquire);
    if (taiko_pc_mode_is_active()) return phase != Phase::Passthrough;
    if (!enabled()) return false;
    return phase != Phase::WaitingForEntry && phase != Phase::Passthrough &&
           phase != Phase::Failed;
}

void publish_preview(std::string_view music_id)
{
    if (!taiko_pc_mode_is_active() || !taiko_pc_mode_is_standalone())
        return;
    std::lock_guard<std::mutex> lock(g_preview_lock);
    if (g_preview_music_id == music_id) return;
    g_preview_music_id.assign(music_id);
    const uint64_t generation =
        g_preview_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    taiko_host_audio_select_preview(g_preview_music_id, generation);
}

// Listing summaries retain installed-course identity and arbitrary osu chart counts.
void browser_course_summary(taiko_overlay_song_row& row, const TaikoCatalogSong& song)
{
    row.course_mask = song.difficulty_mask;
    for (unsigned d=0; d<5; ++d) row.course_stars[d]=song.stars[d];
    if (song.osu_group.empty()) return;
    const auto found=g_osu_groups.find(song.osu_group);
    if (found==g_osu_groups.end()) return;
    row.chart_count=found->second.size();
    for (unsigned d=0; d<std::min(5u,row.chart_count); ++d) {
        const auto* chart=taiko_catalog_song(found->second[d]);
        if (chart) row.chart_stars[d]=chart->stars[3];
    }
}

unsigned normalize_difficulty(const TaikoCatalogSong& song,
                              unsigned preferred)
{
    if (preferred < TAIKO_DIFFICULTY_COUNT &&
        (song.difficulty_mask & (1u << preferred)))
        return preferred;
    unsigned candidate = preferred % TAIKO_DIFFICULTY_COUNT;
    for (unsigned tries = 0; tries < TAIKO_DIFFICULTY_COUNT; ++tries) {
        candidate = (candidate + 1) % TAIKO_DIFFICULTY_COUNT;
        if (song.difficulty_mask & (1u << candidate)) return candidate;
    }
    return TAIKO_DIFFICULTY_ONI;
}

unsigned cycle_difficulty(const TaikoCatalogSong& song,
                          unsigned current, int direction)
{
    unsigned candidate = current % TAIKO_DIFFICULTY_COUNT;
    for (unsigned tries = 0; tries < TAIKO_DIFFICULTY_COUNT; ++tries) {
        candidate = static_cast<unsigned>(
            (static_cast<int>(candidate) + direction +
             static_cast<int>(TAIKO_DIFFICULTY_COUNT)) %
            static_cast<int>(TAIKO_DIFFICULTY_COUNT));
        if (song.difficulty_mask & (1u << candidate)) return candidate;
    }
    return current;
}

std::string searchable_text(const TaikoCatalogSong& song)
{
    std::string value;
    value.reserve(song.title.size() + song.original_title.size() +
                  song.genre.size() + song.music_id.size() + 4);
    value.append(song.title).push_back(' ');
    value.append(song.original_title).push_back(' ');
    value.append(song.genre).push_back(' ');
    value.append(taiko_catalog_genre_name(song.genre)).push_back(' ');
    value.append(song.osu_difficulty).push_back(' ');
    value.append(song.music_id);
    for (char& character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 0x80) character = static_cast<char>(std::tolower(byte));
    }
    return value;
}

bool song_matches_query(const TaikoCatalogSong& song, std::string query)
{
    for (char& character : query) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 0x80) character = static_cast<char>(std::tolower(byte));
    }
    const std::string haystack = searchable_text(song);
    std::size_t position = 0;
    while (position < query.size()) {
        while (position < query.size() &&
               std::isspace(static_cast<unsigned char>(query[position])))
            ++position;
        const std::size_t begin = position;
        while (position < query.size() &&
               !std::isspace(static_cast<unsigned char>(query[position])))
            ++position;
        if (position > begin &&
            haystack.find(query.substr(begin, position - begin)) ==
                std::string::npos)
            return false;
    }
    return true;
}

std::string title_sort_key(const std::string& title)
{
    std::string key;
    key.reserve(title.size());
    for (std::size_t index = 0; index < title.size();) {
        const unsigned char byte = static_cast<unsigned char>(title[index]);
        uint32_t codepoint = byte;
        std::size_t length = 1;
        if ((byte & 0xE0u) == 0xC0u && index + 1 < title.size()) {
            codepoint = ((byte & 0x1Fu) << 6) |
                (static_cast<unsigned char>(title[index + 1]) & 0x3Fu);
            length = 2;
        } else if ((byte & 0xF0u) == 0xE0u && index + 2 < title.size()) {
            codepoint = ((byte & 0x0Fu) << 12) |
                ((static_cast<unsigned char>(title[index + 1]) & 0x3Fu) << 6) |
                (static_cast<unsigned char>(title[index + 2]) & 0x3Fu);
            length = 3;
        } else if ((byte & 0xF8u) == 0xF0u && index + 3 < title.size()) {
            codepoint = ((byte & 0x07u) << 18) |
                ((static_cast<unsigned char>(title[index + 1]) & 0x3Fu) << 12) |
                ((static_cast<unsigned char>(title[index + 2]) & 0x3Fu) << 6) |
                (static_cast<unsigned char>(title[index + 3]) & 0x3Fu);
            length = 4;
        }
        if (codepoint >= 0xFF01 && codepoint <= 0xFF5E)
            codepoint -= 0xFEE0;
        else if (codepoint == 0x3000)
            codepoint = ' ';
        if (codepoint < 0x80) {
            const unsigned char ascii = static_cast<unsigned char>(codepoint);
            if (std::isalnum(ascii))
                key.push_back(static_cast<char>(std::tolower(ascii)));
            else if (ascii == ' ' && !key.empty() && key.back() != ' ')
                key.push_back(' ');
        } else {
            key.append(title, index, length);
        }
        index += length;
    }
    return key;
}

bool custom_folder_browser()
{
    return !g_song_global_search && g_song_query.empty() &&
           (std::string_view(kSongCategories[g_song_category].genre) == "CUSTOM TJA" ||
            std::string_view(kSongCategories[g_song_category].genre) == "NIJIIRO");
}

const char* custom_category_colour(const std::string& folder)
{
    const auto key = title_sort_key(folder);
    if (key == "anime") return "ANIME";
    if (key == "children and folk" || key == "children s songs") return "CHILDREN'S SONGS";
    if (key == "classical") return "CLASSICAL";
    if (key == "game music") return "GAME MUSIC";
    if (key == "namco original") return "NAMCO ORIGINAL";
    if (key == "pop" || key == "j pop") return "J-POP";
    if (key == "variety") return "VARIETY";
    if (key == "vocaloid") return "VOCALOID";
    return folder.c_str();
}

void leave_song_folder_locked()
{
    if (custom_folder_browser() && !g_custom_folder.empty()) {
        const auto slash = g_custom_folder.rfind('/');
        g_custom_folder = slash == std::string::npos ? "" : g_custom_folder.substr(0, slash);
    } else {
        if(!g_song_global_search && g_song_query.empty()) {
            g_open_categories[g_song_category]=false;
            g_open_category_entries[g_song_category].clear();
        }
        g_song_browser_level = SongBrowserLevel::Categories;
        g_custom_folder.clear();
    }
}

void rebuild_song_matches_locked(unsigned preferred_catalog_index)
{
    g_browser_players.collapse();
    g_song_matches.clear();
    g_song_entries.clear();
    ++g_song_entries_generation;
    g_osu_groups.clear();
    for (unsigned i = 0; i < taiko_catalog_count(); ++i) {
        const auto* song = taiko_catalog_song(i);
        if (song && !song->osu_group.empty()) g_osu_groups[song->osu_group].push_back(i);
    }
    for (auto& [group, charts] : g_osu_groups)
        std::stable_sort(charts.begin(), charts.end(), [](unsigned a, unsigned b) {
            const auto* left = taiko_catalog_song(a); const auto* right = taiko_catalog_song(b);
            if (left->stars[3] != right->stars[3]) return left->stars[3] < right->stars[3];
            return left->osu_difficulty < right->osu_difficulty;
        });
    std::unordered_set<std::string> seen_groups;
    std::vector<std::string> folders;
    std::unordered_map<std::string, unsigned> folder_counts;
    const std::size_t count = taiko_catalog_count();
    for (std::size_t index = 0; index < count; ++index) {
        const TaikoCatalogSong* song = taiko_catalog_song(index);
        const SongCategory& category = kSongCategories[g_song_category];
        if (song && (g_song_global_search || song->genre == category.genre) &&
            song_matches_query(*song, g_song_query)) {
            if (custom_folder_browser() && song->custom_folder != g_custom_folder) {
                const auto prefix = g_custom_folder.empty() ? "" : g_custom_folder + "/";
                if (song->custom_folder.compare(0, prefix.size(), prefix) != 0) continue;
                const auto tail = song->custom_folder.substr(prefix.size());
                const auto child = prefix + tail.substr(0, tail.find('/'));
                ++folder_counts[child];
                if (std::find(folders.begin(), folders.end(), child) == folders.end())
                    folders.push_back(child);
                continue;
            }
            if (!song->osu_group.empty()) {
                if (!seen_groups.insert(song->osu_group).second) continue;
                g_song_matches.push_back(g_osu_groups.at(song->osu_group).front());
            } else g_song_matches.push_back(static_cast<unsigned>(index));
        }
    }
    std::stable_sort(g_song_matches.begin(), g_song_matches.end(),
                     [](unsigned left, unsigned right) {
        const TaikoCatalogSong* a = taiko_catalog_song(left);
        const TaikoCatalogSong* b = taiko_catalog_song(right);
        if (!a || !b) return left < right;
        const std::string a_key = title_sort_key(a->title);
        const std::string b_key = title_sort_key(b->title);
        if (a_key != b_key) return a_key < b_key;
        return a->music_id < b->music_id;
    });
    g_song_browser_position = 0;
    std::sort(folders.begin(), folders.end());
    for (const auto& folder : folders)
        g_song_entries.push_back({true, folder_counts[folder], 0, folder});
    // Green opens a stock category on its leading Return card.
    if (!g_song_global_search && g_song_query.empty() && !custom_folder_browser() &&
        !g_song_matches.empty())
        g_song_entries.push_back({true, 0, 0});
    for (unsigned position = 0; position < g_song_matches.size(); ++position) {
        g_song_entries.push_back(
            {false, g_song_matches[position], position});
        if ((position + 1) % 10 == 0 ||
            position + 1 == g_song_matches.size())
            g_song_entries.push_back({true, 0, position});
    }
    if (!folders.empty() && g_song_matches.empty())
        g_song_entries.push_back({true, 0, 0});
    const auto existing = std::find_if(
        g_song_entries.begin(), g_song_entries.end(),
        [preferred_catalog_index](const SongBrowserEntry& entry) {
            return !entry.exit_category &&
                   entry.catalog_index == preferred_catalog_index;
        });
    if (existing != g_song_entries.end())
        g_song_browser_position = static_cast<unsigned>(
            existing - g_song_entries.begin());
}

// Only the current category needs its live catalog/preview state. Neighbours
// retain owning entry lists; publishing eleven rows never flattens a huge library.
void remember_open_category_locked()
{
    if(g_song_browser_level==SongBrowserLevel::Songs && !g_song_global_search &&
       g_song_query.empty() && !custom_folder_browser()) {
        if(!g_open_categories[g_song_category] ||
           g_open_category_generation[g_song_category]!=g_song_entries_generation) {
            g_open_category_entries[g_song_category]=g_song_entries;
            g_open_category_generation[g_song_category]=g_song_entries_generation;
        }
        g_open_categories[g_song_category]=true;
    }
}

bool shared_carousel_locked()
{
    return !g_song_global_search && g_song_query.empty() &&
        !g_song_search_active.load(std::memory_order_relaxed) &&
        (g_song_browser_level==SongBrowserLevel::Categories || !custom_folder_browser());
}

unsigned publish_carousel_rows_locked(
    std::array<taiko_overlay_song_row,TAIKO_OVERLAY_SONG_ROW_COUNT>& rows,
    std::array<std::string,TAIKO_OVERLAY_SONG_ROW_COUNT>& titles,
    std::array<std::string,TAIKO_OVERLAY_SONG_ROW_COUNT>& genres)
{
    auto length=[](unsigned category) -> unsigned {
        return g_open_categories[category] && !g_open_category_entries[category].empty()
            ? static_cast<unsigned>(g_open_category_entries[category].size()) : 1;
    };
    unsigned category=g_song_category;
    unsigned position=g_song_browser_level==SongBrowserLevel::Songs?g_song_browser_position:0;
    for(unsigned step=0;step<TAIKO_OVERLAY_SONG_ROW_COUNT/2;++step) {
        if(position) --position;
        else { category=(category+11)%12;position=length(category)-1; }
    }
    for(unsigned i=0;i<TAIKO_OVERLAY_SONG_ROW_COUNT;++i) {
        auto& row=rows[i];row={};
        row.carousel_group=category+1;row.selected=i==TAIKO_OVERLAY_SONG_ROW_COUNT/2;
        row.browser_position=position;
        genres[i]=kSongCategories[category].label;
        if(g_open_categories[category] && !g_open_category_entries[category].empty()) {
            const auto& entry=g_open_category_entries[category][position];
            row.browser_total=length(category);
            row.catalog_index=entry.catalog_index;
            if(entry.exit_category) { titles[i]="Return";row.kind=TAIKO_OVERLAY_ROW_EXIT;row.catalog_index=position; }
            else {
                const auto* song=taiko_catalog_song(entry.catalog_index);
                titles[i]=song?song->title:"";row.kind=TAIKO_OVERLAY_ROW_SONG;
                if(song)browser_course_summary(row,*song);
            }
        } else {
            titles[i]=kSongCategories[category].label;row.kind=TAIKO_OVERLAY_ROW_CATEGORY;
            std::unordered_set<std::string> groups;
            for(unsigned n=0;n<taiko_catalog_count();++n) {
                const auto* song=taiko_catalog_song(n);
                if(song && song->genre==kSongCategories[category].genre &&
                   (song->osu_group.empty() || groups.insert(song->osu_group).second)) ++row.catalog_index;
            }
        }
        row.title=titles[i].c_str();row.genre=genres[i].c_str();
        if(++position>=length(category)) { position=0;category=(category+1)%12; }
    }
    return TAIKO_OVERLAY_SONG_ROW_COUNT;
}

void show_current_song()
{
    std::lock_guard<std::recursive_mutex> action(g_browser_action_lock);
    taiko_overlay_set_browser_players(taiko_pc_mode_is_standalone() && taiko_pc_mode_is_active(),
        g_browser_players.joined, g_browser_players.ready, g_browser_players.difficulty.data());
    const std::size_t count = taiko_catalog_count();
    if (!count) {
        publish_preview({});
        taiko_overlay_show_song_select(g_session_label);
        return;
    }

    std::array<std::string, TAIKO_OVERLAY_SONG_ROW_COUNT> row_titles;
    std::array<std::string, TAIKO_OVERLAY_SONG_ROW_COUNT> row_genres;
    std::array<taiko_overlay_song_row, TAIKO_OVERLAY_SONG_ROW_COUNT> rows{};
    std::string query;
    unsigned category_index = 0;
    bool category_browser = false;
    {
        std::lock_guard<std::mutex> lock(g_song_browser_lock);
        category_browser =
            g_song_browser_level == SongBrowserLevel::Categories;
        category_index = g_song_category %
            static_cast<unsigned>(kSongCategories.size());
    }
    if (category_browser) {
        g_browser_players.collapse();
        g_player_song_index = ~0u;
        publish_preview({});
        const unsigned category_rows = std::min<unsigned>(TAIKO_OVERLAY_SONG_ROW_COUNT,
            kSongCategories.size());
        const unsigned centre = category_rows / 2;
        const unsigned first_category = (category_index + kSongCategories.size() - centre)
            % kSongCategories.size();
        for (unsigned row = 0; row < category_rows; ++row) {
            const unsigned category = (first_category + row) % kSongCategories.size();
            unsigned category_song_count = 0;
            std::unordered_set<std::string> category_groups;
            for (std::size_t index = 0; index < count; ++index) {
                const TaikoCatalogSong* song = taiko_catalog_song(index);
                if (song && song->genre == kSongCategories[category].genre &&
                    (song->osu_group.empty() || category_groups.insert(song->osu_group).second))
                    ++category_song_count;
            }
            row_titles[row] = kSongCategories[category].label;
            row_genres[row] = kSongCategories[category].label;
            rows[row].title = row_titles[row].c_str();
            rows[row].genre = row_genres[row].c_str();
            rows[row].catalog_index = category_song_count;
            rows[row].selected = category == category_index;
            rows[row].kind = TAIKO_OVERLAY_ROW_CATEGORY;
        }
        {
            std::lock_guard<std::mutex> lock(g_song_browser_lock);
            publish_carousel_rows_locked(rows,row_titles,row_genres);
        }
        taiko_overlay_show_song_browser(
            g_session_label, "", kSongCategories[category_index].label,
            "CATEGORY FOLDER", rows[centre].catalog_index,
            category_index, static_cast<unsigned>(kSongCategories.size()),
            static_cast<unsigned>(count), "CATEGORIES", category_index,
            static_cast<unsigned>(kSongCategories.size()), "", 0, "", 0,
            TAIKO_OVERLAY_BROWSER_CATEGORIES, 0, rows.data(), category_rows);
        return;
    }

    unsigned selection = 0;
    unsigned match_position = 0;
    unsigned match_total = 0;
    unsigned row_count = 0;
    bool selection_is_exit = false;
    std::string browser_category;
    std::string navigation_title;
    unsigned browser_category_index = 0;
    unsigned browser_category_total = 0;
    {
        std::lock_guard<std::mutex> lock(g_song_browser_lock);
        query = g_song_query;
        browser_category = g_song_global_search
            ? "SEARCH RESULTS" : kSongCategories[g_song_category].label;
        if (custom_folder_browser() && !g_custom_folder.empty())
            browser_category += " / " + g_custom_folder;
        browser_category_index = g_song_global_search ? 0 : g_song_category;
        browser_category_total = g_song_global_search
            ? 0 : static_cast<unsigned>(kSongCategories.size());
        match_total = static_cast<unsigned>(g_song_matches.size());
        for (const auto& entry : g_song_entries)
            if (!entry.folder.empty()) match_total += entry.catalog_index;
        if (g_song_entries.empty()) {
            taiko_overlay_show_song_browser(
                g_session_label, "", "", "", 0, 0, 0,
                static_cast<unsigned>(count),
                browser_category.c_str(), browser_category_index,
                browser_category_total, "?", 0,
                query.c_str(),
                g_song_search_active.load(std::memory_order_acquire),
                TAIKO_OVERLAY_BROWSER_SONGS, 0, nullptr, 0);
            return;
        }
        const unsigned entry_total =
            static_cast<unsigned>(g_song_entries.size());
        g_song_browser_position %= entry_total;
        const SongBrowserEntry& current =
            g_song_entries[g_song_browser_position];
        selection_is_exit = current.exit_category;
        navigation_title = current.folder.empty()
            ? (custom_folder_browser() && !g_custom_folder.empty() ? "BACK TO PARENT FOLDER" : "BACK TO CATEGORIES")
            : current.folder.substr(current.folder.rfind('/') + 1);
        match_position = current.song_position;
        if (!selection_is_exit) selection = current.catalog_index;

        const unsigned window_rows = !custom_folder_browser()
            ? TAIKO_OVERLAY_SONG_ROW_COUNT : TAIKO_OVERLAY_LIST_ROW_COUNT;
        row_count = std::min<unsigned>(window_rows, entry_total);
        unsigned first = g_song_browser_position > row_count / 2
            ? g_song_browser_position - row_count / 2 : 0;
        if (first + row_count > entry_total) first = entry_total - row_count;
        for (unsigned row = 0; row < row_count; ++row) {
            const SongBrowserEntry& entry = g_song_entries[first + row];
            rows[row].browser_position=first+row;
            rows[row].browser_total=entry_total;
            if (entry.exit_category) {
                row_titles[row] = entry.folder.empty()
                    ? (custom_folder_browser() && !g_custom_folder.empty() ? "BACK TO PARENT FOLDER" : "BACK TO CATEGORIES")
                    : entry.folder.substr(entry.folder.rfind('/') + 1);
                row_genres[row] = entry.folder.empty() ? browser_category
                    : custom_category_colour(entry.folder);
                rows[row].title = row_titles[row].c_str();
                rows[row].genre = row_genres[row].c_str();
                rows[row].catalog_index = entry.folder.empty() ? first + row + 1 : entry.catalog_index;
                rows[row].selected =
                    first + row == g_song_browser_position;
                rows[row].kind = entry.folder.empty() ? TAIKO_OVERLAY_ROW_EXIT : TAIKO_OVERLAY_ROW_CATEGORY;
                continue;
            }
            const unsigned catalog_index = entry.catalog_index;
            const TaikoCatalogSong* visible =
                taiko_catalog_song(catalog_index);
            if (!visible) continue;
            row_titles[row] = visible->title;
            row_genres[row] = (visible->genre == "CUSTOM TJA" || visible->genre == "NIJIIRO") && !visible->custom_folder.empty()
                ? custom_category_colour(visible->custom_folder) : taiko_catalog_genre_name(visible->genre);
            rows[row].title = row_titles[row].c_str();
            rows[row].genre = row_genres[row].c_str();
            rows[row].catalog_index = entry.song_position;
            rows[row].selected = first + row == g_song_browser_position;
            rows[row].kind = TAIKO_OVERLAY_ROW_SONG;
            browser_course_summary(rows[row],*visible);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_song_browser_lock);
        if(shared_carousel_locked() && !g_browser_players.expanded) {
            remember_open_category_locked();
            row_count=publish_carousel_rows_locked(rows,row_titles,row_genres);
        }
    }

    if (selection_is_exit) {
        g_browser_players.collapse();
        g_player_song_index = ~0u;
        publish_preview({});
        taiko_overlay_show_song_browser(
            g_session_label, "", navigation_title.c_str(),
            browser_category.c_str(), 0, match_position,
            match_total, static_cast<unsigned>(count),
            browser_category.c_str(), browser_category_index,
            browser_category_total, "", 0,
            query.c_str(),
            g_song_search_active.load(std::memory_order_acquire),
            TAIKO_OVERLAY_BROWSER_SONGS, 1, rows.data(), row_count);
        return;
    }

    const TaikoCatalogSong* song = taiko_catalog_song(selection);
    if (!song) return;
    if (g_player_song_index != selection) {
        g_browser_players.song_changed(song->difficulty_mask);
        g_player_song_index = selection;
        g_osu_variant = 0;
    } else g_browser_players.normalize(song->difficulty_mask);
    taiko_overlay_set_browser_players(taiko_pc_mode_is_standalone() && taiko_pc_mode_is_active(),
        g_browser_players.joined, g_browser_players.ready, g_browser_players.difficulty.data());
    if (g_browser_players.expanded && !song->osu_group.empty()) {
        const auto& charts = g_osu_groups.at(song->osu_group);
        g_osu_variant %= charts.size();
        unsigned parent = 0;
        while (parent < row_count && !rows[parent].selected) ++parent;
        const auto header = rows[parent];
        rows = {};
        rows[0] = header;
        row_count = 1;
        const unsigned visible = std::min<unsigned>(charts.size(), TAIKO_OVERLAY_LIST_ROW_COUNT - 1);
        unsigned first = g_osu_variant > visible / 2 ? g_osu_variant - visible / 2 : 0;
        if (first + visible > charts.size()) first = charts.size() - visible;
        for (unsigned d = first; d < first + visible; ++d) {
            const auto* chart = taiko_catalog_song(charts[d]);
            auto& row = rows[row_count++];
            row.title = chart->osu_difficulty.c_str();
            row.genre = "";
            row.catalog_index = charts[d];
            row.kind = TAIKO_OVERLAY_ROW_DIFFICULTY;
            row.difficulty = 3;
            row.stars = chart->stars[3];
            row.selected = d == g_osu_variant;
            row.cursors = row.selected ? g_browser_players.joined : 0;
            row.ready = g_browser_players.ready & row.cursors;
        }
        song = taiko_catalog_song(charts[g_osu_variant]);
    } else if (g_browser_players.expanded) {
        unsigned parent = 0;
        while (parent < row_count && !rows[parent].selected) ++parent;
        // Keep the song header and every installed stock course together,
        // including at either end of the library. Course identity lives on
        // the row; the overlay never infers it from a visible row number.
        const unsigned preceding = std::min(parent, 2u);
        const unsigned start = parent - preceding;
        auto original = rows;
        row_count = 0;
        for (unsigned r = start; r <= parent; ++r) rows[row_count++] = original[r];
        for (unsigned d = 0; d < TAIKO_DIFFICULTY_COUNT; ++d) {
            if (!(song->difficulty_mask & (1u << d))) continue;
            auto& row = rows[row_count++];
            row = {};
            row.title = taiko_catalog_difficulty_name(d);
            row.genre = "";
            row.catalog_index = selection;
            row.kind = TAIKO_OVERLAY_ROW_DIFFICULTY;
            row.difficulty = d;
            row.stars = song->stars[d];
            row.cursors = taiko_pc_mode_is_standalone() && taiko_pc_mode_is_active()
                ? g_browser_players.cursors(d)
                : (g_song_difficulty.load() == d ? 1 : 0);
            row.ready = g_browser_players.ready & row.cursors;
            row.selected = row.cursors != 0;
        }
        for (unsigned r = parent + 1; r < TAIKO_OVERLAY_LIST_ROW_COUNT &&
             row_count < TAIKO_OVERLAY_LIST_ROW_COUNT && original[r].title; ++r)
            rows[row_count++] = original[r];
    }
    // The browser entry is the stable representative of an osu set/audio
    // group. Changing its selected chart must not replace the preview voice.
    // Gameplay still uses the selected chart's own identity below/on launch.
    publish_preview(song->osu_group.empty() ? song->music_id
                                           : taiko_catalog_song(selection)->music_id);
    unsigned difficulty = g_song_difficulty.load(std::memory_order_acquire);
    difficulty = normalize_difficulty(*song, difficulty);
    g_song_selection.store(selection, std::memory_order_release);
    g_song_difficulty.store(difficulty, std::memory_order_release);
    taiko_overlay_show_song_browser(
        g_session_label, song->music_id.c_str(), song->title.c_str(),
        taiko_catalog_genre_name(song->genre), song->unique_id, match_position,
        match_total, static_cast<unsigned>(count),
        browser_category.c_str(), browser_category_index,
        browser_category_total,
        taiko_catalog_difficulty_name(difficulty), song->difficulty_mask,
        query.c_str(),
        g_song_search_active.load(std::memory_order_acquire),
        TAIKO_OVERLAY_BROWSER_SONGS, 0, rows.data(), row_count);
}

void enter_song_select_shell()
{
    std::lock_guard<std::recursive_mutex> action(g_browser_action_lock);
    g_browser_players.collapse();
    g_song_launch_requested.store(false, std::memory_order_release);
    (void)taiko_catalog_load();
    {
        std::lock_guard<std::mutex> lock(g_song_browser_lock);
        g_song_query.clear();
        g_song_browser_level = SongBrowserLevel::Categories;
        g_song_global_search = false;
        g_song_browser_position = 0;
        g_open_categories.fill(false);
        for(auto& entries:g_open_category_entries) entries.clear();
    }
    g_song_search_active.store(false, std::memory_order_release);
    show_current_song();
}

extern "C" void taiko_frontend_enter_song_select_shell(void)
{
    std::lock_guard<std::recursive_mutex> action(g_browser_action_lock);
    g_phase.store(Phase::SongSelect, std::memory_order_release);
    enter_song_select_shell();
    taiko_overlay_animate_browser(0);
}

void change_song_category(int direction)
{
    const unsigned count = static_cast<unsigned>(kSongCategories.size());
    {
        std::lock_guard<std::mutex> lock(g_song_browser_lock);
        if (g_song_browser_level != SongBrowserLevel::Categories) return;
        int next = static_cast<int>(g_song_category) + direction;
        next %= static_cast<int>(count);
        if (next < 0) next += static_cast<int>(count);
        g_song_category = static_cast<unsigned>(next);
        if(g_open_categories[g_song_category]) {
            g_song_browser_level=SongBrowserLevel::Songs;
            rebuild_song_matches_locked(~0u);
            g_song_browser_position=direction<0 && !g_song_entries.empty()?g_song_entries.size()-1:0;
        }
    }
    browser_sfx(TaikoPlusSfx::Move);
    show_current_song();
}

void change_song_difficulty(int direction, unsigned player = 2);

void move_song_selection(int delta, unsigned player = 2)
{
    if (g_browser_players.expanded) {
        change_song_difficulty(delta < 0 ? -1 : 1, player);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_song_browser_lock);
        if(shared_carousel_locked()) {
            remember_open_category_locked();
            int direction=delta<0?-1:1;
            for(int step=0;step<std::abs(delta);++step) {
                int next=static_cast<int>(g_song_browser_position)+direction;
                if(g_song_browser_level==SongBrowserLevel::Songs && next>=0 &&
                   next<static_cast<int>(g_song_entries.size())) {
                    g_song_browser_position=static_cast<unsigned>(next);
                    continue;
                }
                g_song_category=(g_song_category+12+direction)%12;
                g_custom_folder.clear();
                if(g_open_categories[g_song_category]) {
                    g_song_browser_level=SongBrowserLevel::Songs;
                    rebuild_song_matches_locked(~0u);
                    g_song_browser_position=direction>0 || g_song_entries.empty()?0:
                        static_cast<unsigned>(g_song_entries.size()-1);
                    remember_open_category_locked();
                } else {
                    g_song_browser_level=SongBrowserLevel::Categories;
                    g_song_browser_position=0;
                }
            }
        } else {
            const int count = static_cast<int>(g_song_entries.size());
            if (!count) return;
            int next = (static_cast<int>(g_song_browser_position) + delta)%count;
            if (next < 0) next += count;
            g_song_browser_position = static_cast<unsigned>(next);
        }
    }
    browser_sfx(TaikoPlusSfx::Move);
    show_current_song();
}

void select_song_endpoint(bool last)
{
    if (g_browser_players.expanded) return;
    {
        std::lock_guard<std::mutex> lock(g_song_browser_lock);
        if (g_song_browser_level == SongBrowserLevel::Categories) {
            g_song_category = last
                ? static_cast<unsigned>(kSongCategories.size() - 1) : 0;
            if(g_open_categories[g_song_category]) {
                g_song_browser_level=SongBrowserLevel::Songs;
                rebuild_song_matches_locked(~0u);
                g_song_browser_position=last && !g_song_entries.empty()?g_song_entries.size()-1:0;
            }
        } else {
            if (g_song_entries.empty()) return;
            g_song_browser_position = last
                ? static_cast<unsigned>(g_song_entries.size() - 1) : 0;
        }
    }
    browser_sfx(TaikoPlusSfx::Move);
    show_current_song();
}

void select_random_song()
{
    g_browser_players.collapse();
    uint32_t state = g_random_state.load(std::memory_order_relaxed);
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    g_random_state.store(state, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_song_browser_lock);
        if (g_song_browser_level != SongBrowserLevel::Songs ||
            g_song_matches.empty()) return;
        const unsigned catalog_index = g_song_matches[
            state % static_cast<unsigned>(g_song_matches.size())];
        const auto entry = std::find_if(
            g_song_entries.begin(), g_song_entries.end(),
            [catalog_index](const SongBrowserEntry& candidate) {
                return !candidate.exit_category &&
                       candidate.catalog_index == catalog_index;
            });
        if (entry != g_song_entries.end())
            g_song_browser_position = static_cast<unsigned>(
                entry - g_song_entries.begin());
    }
    browser_sfx(TaikoPlusSfx::Move);
    show_current_song();
}

void change_song_difficulty(int direction, unsigned player)
{
    if (!g_browser_players.expanded) return;
    unsigned selection = 0;
    {
        std::lock_guard<std::mutex> lock(g_song_browser_lock);
        if (g_song_browser_level != SongBrowserLevel::Songs ||
            g_song_entries.empty() ||
            g_song_entries[g_song_browser_position].exit_category)
            return;
        selection = g_song_entries[g_song_browser_position].catalog_index;
    }
    const TaikoCatalogSong* song = taiko_catalog_song(selection);
    if (!song) return;
    if (!song->osu_group.empty()) {
        const auto size = g_osu_groups.at(song->osu_group).size();
        g_osu_variant = (int64_t(g_osu_variant) + direction + size) % size;
        if (player < 2) g_browser_players.join(player);
        g_browser_players.ready = 0;
    } else if (taiko_pc_mode_is_standalone() && taiko_pc_mode_is_active()) {
        if (player > 1) player = g_browser_players.focus;
        g_browser_players.change_difficulty(player, direction, song->difficulty_mask);
        g_song_difficulty.store(g_browser_players.difficulty[player], std::memory_order_release);
    } else {
        const unsigned current = g_song_difficulty.load(std::memory_order_relaxed);
        g_song_difficulty.store(cycle_difficulty(*song, current, direction),
                                std::memory_order_release);
    }
    browser_sfx(TaikoPlusSfx::Difficulty);
    show_current_song();
}

void request_song_launch(unsigned player = 2)
{
    unsigned selection = 0;
    {
        std::lock_guard<std::mutex> lock(g_song_browser_lock);
        if (g_song_browser_level != SongBrowserLevel::Songs ||
            g_song_entries.empty() ||
            g_song_entries[g_song_browser_position].exit_category)
            return;
        selection = g_song_entries[g_song_browser_position].catalog_index;
    }
    const TaikoCatalogSong* song = taiko_catalog_song(selection);
    if (song && !song->osu_group.empty()) {
        const auto& charts = g_osu_groups.at(song->osu_group);
        selection = charts[g_osu_variant % charts.size()];
        song = taiko_catalog_song(selection);
    }
    if (!song || g_song_launch_requested.load(std::memory_order_acquire))
        return;
    const unsigned difficulty =
        g_song_difficulty.load(std::memory_order_relaxed);

    if (taiko_pc_mode_is_active() && taiko_pc_mode_is_standalone()) {
        if (player > 1) player = g_browser_players.focus;
        if (!g_browser_players.confirm(player)) {
            show_current_song();
            return;
        }
        taiko_plus::MatchConfig pending;
        for (unsigned slot = 0; slot < 2; ++slot) {
            auto& spec = pending.players[slot];
            spec.slot = static_cast<taiko_plus::PlayerSlot>(slot);
            spec.role = taiko_plus::PlayerRole::Local;
            spec.enabled = (g_browser_players.joined & (1u << slot)) != 0;
            spec.difficulty = g_browser_players.difficulty[slot];
        }
        if (g_song_launch_requested.exchange(true, std::memory_order_acq_rel)) return;
        pending.generation = taiko_plus::runtime().generation();
        static std::future<void> preparation;
        auto prepare = [selection, pending]() mutable {
            std::string error;
            bool first = true;
            try {
                for (auto& spec : pending.players) {
                    if (!spec.enabled) continue;
                    taiko_plus::ContentIdentity content;
                    if (!taiko_catalog_content_identity(selection, spec.difficulty, content, &error)) {
                        taiko_frontend_standalone_failure(error.c_str());
                        return;
                    }
                    spec.chart_hash = content.chart_hash;
                    if (first) { pending.content = std::move(content); first = false; }
                }
                if (!taiko_plus::runtime().enqueue_launch(std::move(pending)))
                    taiko_frontend_standalone_failure("launch queue rejected prepared song");
            } catch (const std::exception& e) {
                taiko_frontend_standalone_failure(e.what());
            }
        };
        if (song->tja_path.empty()) prepare();
        else {
            taiko_overlay_set_browser_save_status("Preparing custom song...");
            preparation = std::async(std::launch::async, std::move(prepare));
        }
        browser_sfx(TaikoPlusSfx::Confirm);
        std::fprintf(stderr,
                     "[taiko_frontend] standalone launch queued id=%s mask=%u courses=%u/%u\n",
                     song->music_id.c_str(), g_browser_players.joined,
                     g_browser_players.difficulty[0], g_browser_players.difficulty[1]);
        show_current_song();
        return;
    }

    if (g_song_launch_requested.exchange(true, std::memory_order_acq_rel))
        return;
    browser_sfx(TaikoPlusSfx::Confirm);
    std::fprintf(stderr,
                 "[taiko_frontend] launch requested id=%s difficulty=%u\n",
                 song->music_id.c_str(), difficulty);
    show_current_song();
}

void close_global_search_locked()
{
    g_song_global_search=false;g_song_query.clear();
    g_song_browser_level=g_search_return_level;
    rebuild_song_matches_locked(g_search_return_selection);
    if(!g_song_entries.empty())g_song_browser_position=std::min<unsigned>(g_search_return_position,g_song_entries.size()-1);
}

void activate_browser_selection(unsigned player = 2)
{
    bool launch_song = false;
    bool leaving_song_list = false;
    {
        std::lock_guard<std::mutex> lock(g_song_browser_lock);
        if (g_song_browser_level == SongBrowserLevel::Categories) {
            g_song_browser_level = SongBrowserLevel::Songs;
            g_song_global_search = false;
            g_song_query.clear();
            rebuild_song_matches_locked(custom_folder_browser()
                ? g_song_selection.load(std::memory_order_acquire) : ~0u);
            g_song_search_active.store(false, std::memory_order_release);
            remember_open_category_locked();
        } else if (!g_song_entries.empty() &&
                   !g_song_entries[g_song_browser_position].folder.empty()) {
            g_custom_folder = g_song_entries[g_song_browser_position].folder;
            rebuild_song_matches_locked(~0u);
        } else if (!g_song_entries.empty() &&
                   g_song_entries[g_song_browser_position].exit_category) {
            if(g_song_global_search)close_global_search_locked();
            else {leave_song_folder_locked();rebuild_song_matches_locked(~0u);}
            g_song_global_search = false;
            g_song_query.clear();
            g_song_search_active.store(false, std::memory_order_release);
            leaving_song_list = true;
        } else if (!g_song_entries.empty()) {
            const auto* song = taiko_catalog_song(g_song_entries[g_song_browser_position].catalog_index);
            if (song && song->difficulty_mask) {
                launch_song = g_browser_players.expanded;
                if (!launch_song) {
                    if (taiko_pc_mode_is_active() && taiko_pc_mode_is_standalone())
                        g_browser_players.join(player < 2 ? player : g_browser_players.focus);
                    g_browser_players.open(song->difficulty_mask);
                }
            }
        }
    }
    if (launch_song)
        request_song_launch(player);
    else {
        browser_sfx(leaving_song_list
            ? TaikoPlusSfx::Cancel : TaikoPlusSfx::Confirm);
        show_current_song();
    }
}

void handle_rising(unsigned player, uint32_t rising)
{
    if (player > 1 || !rising) return;
    std::lock_guard<std::recursive_mutex> action(g_browser_action_lock);
    const Phase phase = g_phase.load(std::memory_order_acquire);
    if (phase == Phase::SongSelect) {
        if (g_song_launch_requested.load(std::memory_order_acquire)) return;
        if (g_browser_accounts.busy()) {
            if (g_browser_accounts.phase == taiko_plus::AccountPhase::ChoosePlayer &&
                (rising & (TAIKO_ACTION_HIT_CL | TAIKO_ACTION_HIT_CR | TAIKO_ACTION_ENTER))) {
                g_browser_accounts.assign(player);
                show_browser_login();
            }
            return;
        }
        if (taiko_pc_mode_is_active() && taiko_pc_mode_is_standalone())
            g_browser_players.join(player);
        else if (player != 0) return;
        const bool rim = rising & (TAIKO_ACTION_HIT_SL | TAIKO_ACTION_HIT_SR);
        const bool centre = rising & (TAIKO_ACTION_HIT_CL | TAIKO_ACTION_HIT_CR);
        if (rim) taiko_host_audio_play_sfx(TaikoPlusSfx::Move);
        if (centre) taiko_host_audio_play_sfx(TaikoPlusSfx::Confirm);
        struct DrumFeedbackScope {
            bool saved = g_drum_feedback;
            explicit DrumFeedbackScope(bool active) { g_drum_feedback = active; }
            ~DrumFeedbackScope() { g_drum_feedback = saved; }
        } feedback(rim || centre);
        const std::size_t count = taiko_catalog_count();
        if (!count) return;
        if (rising & TAIKO_ACTION_UP)
            move_song_selection(-TAIKO_OVERLAY_LIST_ROW_COUNT + 1, player);
        else if (rising & TAIKO_ACTION_DOWN)
            move_song_selection(TAIKO_OVERLAY_LIST_ROW_COUNT - 1, player);
        else if (rising & TAIKO_ACTION_HIT_SL)
            move_song_selection(-1, player);
        else if (rising & TAIKO_ACTION_HIT_SR)
            move_song_selection(1, player);

        if (rising & TAIKO_ACTION_HIT_CL) {
            if (g_browser_players.expanded) {
                g_browser_players.collapse();
                browser_sfx(TaikoPlusSfx::Cancel);
            } else taiko_frontend_browser_command(TAIKO_BROWSER_SEARCH_CLEAR);
        } else if (rising & (TAIKO_ACTION_HIT_CR | TAIKO_ACTION_ENTER))
            activate_browser_selection(player);
        show_current_song();
        return;
    }
    if (player != 0 || phase != Phase::LoginMenu) return;

    const uint32_t rims = TAIKO_ACTION_HIT_SL | TAIKO_ACTION_HIT_SR |
                          TAIKO_ACTION_UP | TAIKO_ACTION_DOWN;
    const uint32_t centres = TAIKO_ACTION_HIT_CL | TAIKO_ACTION_HIT_CR |
                             TAIKO_ACTION_ENTER;
    if (rising & rims) {
        const unsigned selection = g_selection.load(std::memory_order_relaxed) ^ 1u;
        g_selection.store(selection, std::memory_order_release);
        taiko_overlay_show_entry_menu((int)selection);
    }
    if (!(rising & centres)) return;

    if (g_selection.load(std::memory_order_acquire) == 0) {
        g_phase.store(Phase::AnonymousRequested, std::memory_order_release);
        std::fprintf(stderr, "[taiko_frontend] anonymous requested\n");
    } else {
        g_card_select_ticks = 0;
        g_phase.store(Phase::BaidWaiting, std::memory_order_release);
        taiko_overlay_show_baid_wait();
        std::fprintf(stderr, "[taiko_frontend] BanaPassport login requested\n");
    }
}

void write_integer_variant(uint32_t address, uint32_t value)
{
    /* Raw callback value type 3 is integer. func_00397C04 normalizes it to
     * result type 2 for func_00399074's callers. */
    vm_write32(address, 3);
    vm_write32(address + 4, value);
}

bool invoke_player_join(uint32_t controller, uint32_t toc,
                        bool require_offline_record)
{
    std::array<uint8_t, kPlayerRecordSize> before{};
    const uint32_t record = controller + kPlayerRecordOffset;
    for (uint32_t offset = 0; offset < kPlayerRecordSize; ++offset)
        before[offset] = vm_read8(record + offset);

    if (!require_offline_record) {
        /* Stock Card Select first assigns the decoded BAID staging record
         * (slot 2) to the chosen cabinet player through func_00225CB8. Its
         * sole argument is the destination player index. Confirming through
         * func_00226A9C alone merely activates the default no-card record. */
        for (uint32_t offset = 0; offset < 0x40; offset += 4)
            vm_write32(kCallbackFrame + offset, 0);
        for (uint32_t offset = 0; offset < 0x110; offset += 4)
            vm_write32(kCallbackArgument2 + offset, 0);
        write_integer_variant(kCallbackArgument1, 0); /* assign BAID to P1 */
        vm_write32(kCallbackFrame + 0x10, kCallbackStack);
        vm_write32(kCallbackStack + 0x20, 8);
        vm_write32(kCallbackStack + 0x28, 1);
        vm_write32(kCallbackStack + 0x2C, 10);
        vm_write32(kCallbackFrame + 0x14, kCallbackArgument1);
        vm_write32(kCallbackFrame + 0x18, kCallbackBufferEnd);
        vm_write32(kCallbackFrame + 0x1C, kCallbackArgument1);
        ppu_guest_call_ct(kAuthenticatedProfileCopy, toc,
                          kCallbackFrame, 0, 0, 0);

        unsigned profile_changed = 0;
        for (uint32_t offset = 0; offset < kPlayerRecordSize; ++offset)
            profile_changed += before[offset] != vm_read8(record + offset);
        std::fprintf(stderr,
                     "[taiko_frontend] authenticated profile assignment "
                     "record=%08X changed=%u\n",
                     record, profile_changed);
        if (profile_changed < 16)
            return false;
    }

    for (uint32_t offset = 0; offset < 0x40; offset += 4)
        vm_write32(kCallbackFrame + offset, 0);
    for (uint32_t offset = 0; offset < 0x110; offset += 4)
        vm_write32(kCallbackArgument2 + offset, 0);

    write_integer_variant(kCallbackArgument1, 0); /* P1 */
    /* Both stock no-card and authenticated Card Select captures pass zero.
     * The profile source is selected by the native manager context populated
     * by BAID, not by this callback argument. */
    write_integer_variant(kCallbackArgument2, 0);
    /* func_00399074 takes the argument count from the auxiliary callback
     * stack at frame[0x10] + 0x28, not from the outer frame itself. */
    vm_write32(kCallbackFrame + 0x10, kCallbackStack);
    vm_write32(kCallbackStack + 0x20, 8);
    vm_write32(kCallbackStack + 0x28, 2);
    vm_write32(kCallbackStack + 0x2C, 10);
    vm_write32(kCallbackFrame + 0x14, kCallbackArgument2);
    vm_write32(kCallbackFrame + 0x18, kCallbackBufferEnd);
    vm_write32(kCallbackFrame + 0x1C, kCallbackArgument1);

    ppu_guest_call_ct(kAnonymousJoin, toc, kCallbackFrame, 0, 0, 0);

    const uint8_t active = vm_read8(record);
    const uint8_t kind = vm_read8(record + 0x42B);
    unsigned changed = 0;
    for (uint32_t offset = 0; offset < kPlayerRecordSize; ++offset)
        changed += before[offset] != vm_read8(record + offset);
    std::fprintf(stderr,
                 "[taiko_frontend] player join transaction mode=%s "
                 "record=%08X active=%u kind=%u changed=%u\n",
                 require_offline_record ? "offline" : "authenticated",
                 record, active, kind, changed);
    if (active != 1) return false;
    if (require_offline_record && kind != 2) return false;
    /* A real BAID selection copies the account identity, unlock/profile and
     * costume fields into the 0x4f0-byte player record. The captured account
     * changed 151 bytes; the premature CardSelect call changed only the two
     * anonymous/session bytes. Never label that fallback as authenticated. */
    if (!require_offline_record && changed < 16) return false;

    return true;
}

bool invoke_game_mode_commit(uint32_t controller, uint32_t toc)
{
    for (uint32_t offset = 0; offset < 0x80; offset += 4)
        vm_write32(kCallbackFrame + offset, 0);
    for (uint32_t offset = 0; offset < 0x120; offset += 4)
        vm_write32(kCallbackArgument3 + offset, 0);

    /* Captured from the stock anonymous one-player confirmation callback. */
    write_integer_variant(kCallbackArgument1, 1);
    write_integer_variant(kCallbackArgument2, 0);
    write_integer_variant(kCallbackArgument3, 0);
    vm_write32(kCallbackFrame + 0x10, kCallbackStack);
    vm_write32(kCallbackStack + 0x20, 8);
    vm_write32(kCallbackStack + 0x28, 3);
    vm_write32(kCallbackStack + 0x2C, 10);
    vm_write32(kCallbackFrame + 0x14, kCallbackArgument3);
    vm_write32(kCallbackFrame + 0x18, kCallbackBufferEnd);
    vm_write32(kCallbackFrame + 0x1C, kCallbackArgument1);

    ppu_guest_call_ct(kGameModeCommit, toc, kCallbackFrame, 0, 0, 0);

    const uint32_t vector = controller + kPlayerRecordOffset + 0x4C0;
    const uint32_t begin = vm_read32(vector + 4);
    const uint32_t end = vm_read32(vector + 8);
    const uint32_t capacity = vm_read32(vector + 12);
    std::fprintf(stderr,
                 "[taiko_frontend] game-mode transaction vector=%08X "
                 "begin=%08X end=%08X capacity=%08X\n",
                 vector, begin, end, capacity);
    return begin && end == begin + 12 && capacity >= end;
}

bool begin_native_handoff(uint32_t controller, uint32_t toc)
{
    if (!invoke_game_mode_commit(controller, toc))
        return false;

    /* The stock branch at 0x001F3428 starts the embedded userdata controller
     * before entering the common tail. Its completion state is the gate from
     * UserData_WaitServer to state 27 even when no HTTP request is required. */
    if (vm_read8(controller + kOnlineCompletionFlagOffset) == 0) {
        ppu_guest_call_ct(kUserDataBegin, toc,
                          controller + kUserDataControllerOffset, 0, 0, 0);
        std::fprintf(stderr,
                     "[taiko_frontend] started native userdata "
                     "controller %08X\n",
                     controller + kUserDataControllerOffset);
    }

    /* This is the common stock tail at 0x001F0560: a 60-frame grace interval,
     * reset the entry subcontroller, then enter WaitEndInterval. */
    vm_write32(controller + kIntervalOffset, 0x3C);
    ppu_guest_call_ct(kEntrySubcontrollerReset, toc,
                      controller + kEntrySubcontrollerOffset, 0, 0, 0);
    ppu_guest_call_ct(kEntrySetState, toc, controller,
                      kStateWaitEndInterval, 0, 0);
    std::fprintf(stderr,
                 "[taiko_frontend] native handoff requested state=%u\n",
                 kStateWaitEndInterval);
    return true;
}

void release_to_stock(const char* reason)
{
    g_phase.store(Phase::Passthrough, std::memory_order_release);
    taiko_overlay_hide_host_screen();
    std::fprintf(stderr,
                 "[taiko_frontend] released input to stock Player Entry: %s\n",
                 reason);
}

bool guest_string_equals(uint32_t object, const std::string& expected)
{
    const uint32_t length = vm_read32(object + 0x10);
    const uint32_t capacity = vm_read32(object + 0x14);
    if (length != expected.size() || length > 64) return false;
    const uint32_t data = capacity <= 15 ? object : vm_read32(object);
    if (!data) return false;
    for (uint32_t i = 0; i < length; ++i) {
        if (vm_read8(data + i) != static_cast<uint8_t>(expected[i]))
            return false;
    }
    return true;
}

bool find_live_song(uint32_t manager, const std::string& music_id,
                    uint32_t* index_out, uint32_t* record_out)
{
    const uint32_t begin = vm_read32(manager + kSongVectorBeginOffset);
    const uint32_t end = vm_read32(manager + kSongVectorEndOffset);
    if (!begin || end < begin || (end - begin) % kSongRecordSize != 0)
        return false;
    const uint32_t count = (end - begin) / kSongRecordSize;
    if (!count || count > 2048) return false;
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t record = begin + index * kSongRecordSize;
        if (!guest_string_equals(record + kSongRecordMusicIdOffset, music_id))
            continue;
        if (index_out) *index_out = index;
        if (record_out) *record_out = record;
        return true;
    }
    return false;
}

} // namespace

extern "C" int taiko_frontend_consume_press(unsigned player, uint32_t actions)
{
    if (!frontend_owns_input()) return 0;
    handle_rising(player, actions);
    return 1;
}

extern "C" int taiko_frontend_consume_release(unsigned player,
                                                uint32_t actions)
{
    (void)player;
    (void)actions;
    return frontend_owns_input() ? 1 : 0;
}

extern "C" uint32_t taiko_frontend_filter_levels(unsigned player,
                                                   uint32_t levels,
                                                   uint64_t timestamp_ns)
{
    (void)timestamp_ns;
    if (player >= g_raw_levels.size()) return levels;
    const uint32_t previous = g_raw_levels[player].exchange(
        levels, std::memory_order_acq_rel);
    if (!frontend_owns_input()) return levels;
    handle_rising(player, levels & ~previous);
    return 0;
}

extern "C" int taiko_frontend_browser_command(unsigned command)
{
    std::lock_guard<std::recursive_mutex> action(g_browser_action_lock);
    if (g_phase.load(std::memory_order_acquire) != Phase::SongSelect ||
        g_song_launch_requested.load(std::memory_order_acquire))
        return 0;

    if (g_browser_accounts.busy() || g_browser_accounts.phase == taiko_plus::AccountPhase::Failed) {
        if (command == TAIKO_BROWSER_SEARCH_CLEAR) cancel_browser_login();
        else if (g_browser_accounts.phase == taiko_plus::AccountPhase::ChoosePlayer &&
                 (command == TAIKO_BROWSER_PLAYER1_TOGGLE || command == TAIKO_BROWSER_PLAYER2_TOGGLE)) {
            g_browser_accounts.assign(command == TAIKO_BROWSER_PLAYER1_TOGGLE ? 0 : 1);
            show_browser_login();
        }
        return 1;
    }
    switch (command) {
    case TAIKO_BROWSER_ACCOUNT_LOGIN: {
        if (!taiko_pc_mode_is_active() || !taiko_pc_mode_is_standalone()) return 0;
        const auto token = g_browser_accounts.begin();
        if (!token) return 1;
        g_browser_card_lease = taiko_card_browser_begin();
        if (!g_browser_card_lease) g_browser_accounts.fail(token, "Reader busy - try again");
        else g_browser_accounts.status = "Waiting for pairing PIN...";
        g_song_search_active.store(false, std::memory_order_release);
        g_browser_players.collapse();
        show_current_song();
        show_browser_login();
        break;
    }
    case TAIKO_BROWSER_PLAYER1_TOGGLE:
    case TAIKO_BROWSER_PLAYER2_TOGGLE: {
        if (!taiko_pc_mode_is_active() || !taiko_pc_mode_is_standalone()) return 0;
        if (g_browser_accounts.busy()) return 1;
        const unsigned slot = command == TAIKO_BROWSER_PLAYER1_TOGGLE ? 0 : 1;
        const bool leaving = g_browser_players.joined & (1u << slot);
        if (leaving) g_browser_players.leave(slot);
        else g_browser_players.join(slot);
        browser_sfx(leaving ? TaikoPlusSfx::Cancel : TaikoPlusSfx::Confirm);
        show_current_song();
        break;
    }
    case TAIKO_BROWSER_SEARCH_TOGGLE: {
        g_browser_players.collapse();
        const bool editing=g_song_search_active.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_song_browser_lock);
            if(!g_song_global_search) {
                g_search_return_level=g_song_browser_level;
                g_search_return_position=g_song_browser_position;
                g_search_return_selection=g_song_selection.load(std::memory_order_acquire);
            }
            g_song_browser_level = SongBrowserLevel::Songs;
            g_song_global_search = true;
            if(!editing) rebuild_song_matches_locked(g_song_selection.load(std::memory_order_acquire));
        }
        g_song_search_active.store(!editing,std::memory_order_release);
        show_current_song();
        break;
    }
    case TAIKO_BROWSER_SEARCH_CLEAR: {
        if (g_browser_players.expanded) {
            g_browser_players.collapse();
            browser_sfx(TaikoPlusSfx::Cancel);
            show_current_song();
            break;
        }
        const unsigned selected =
            g_song_selection.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(g_song_browser_lock);
            if (g_song_browser_level == SongBrowserLevel::Categories)
                return 0;
            if(g_song_global_search)close_global_search_locked();
            else if (g_song_search_active.load(std::memory_order_relaxed) ||
                !g_song_query.empty()) {
                g_song_query.clear();
                g_song_global_search=false;
                rebuild_song_matches_locked(selected);
            } else {
                leave_song_folder_locked();
                g_song_global_search = false;
                rebuild_song_matches_locked(~0u);
            }
        }
        g_song_search_active.store(false, std::memory_order_release);
        show_current_song();
        break;
    }
    case TAIKO_BROWSER_SEARCH_BACKSPACE: {
        if (!g_song_search_active.load(std::memory_order_acquire)) return 0;
        const unsigned selected =
            g_song_selection.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(g_song_browser_lock);
            if (!g_song_query.empty()) {
                std::size_t start = g_song_query.size() - 1;
                while (start > 0 &&
                       (static_cast<unsigned char>(g_song_query[start]) &
                        0xC0u) == 0x80u)
                    --start;
                g_song_query.erase(start);
            }
            rebuild_song_matches_locked(selected);
        }
        show_current_song();
        break;
    }
    case TAIKO_BROWSER_PREVIOUS:
        move_song_selection(-1);
        break;
    case TAIKO_BROWSER_NEXT:
        move_song_selection(1);
        break;
    case TAIKO_BROWSER_PREVIOUS_PAGE:
        move_song_selection(-TAIKO_OVERLAY_LIST_ROW_COUNT + 1);
        break;
    case TAIKO_BROWSER_NEXT_PAGE:
        move_song_selection(TAIKO_OVERLAY_LIST_ROW_COUNT - 1);
        break;
    case TAIKO_BROWSER_FIRST:
        select_song_endpoint(false);
        break;
    case TAIKO_BROWSER_LAST:
        select_song_endpoint(true);
        break;
    case TAIKO_BROWSER_RANDOM:
        select_random_song();
        break;
    case TAIKO_BROWSER_CATEGORY_PREVIOUS:
        change_song_category(-1);
        break;
    case TAIKO_BROWSER_CATEGORY_NEXT:
        change_song_category(1);
        break;
    case TAIKO_BROWSER_DIFFICULTY_PREVIOUS:
        change_song_difficulty(-1);
        break;
    case TAIKO_BROWSER_DIFFICULTY_NEXT:
        change_song_difficulty(1);
        break;
    case TAIKO_BROWSER_PLAY:
        if (g_song_search_active.exchange(false, std::memory_order_acq_rel))
            show_current_song();
        else
            activate_browser_selection();
        break;
    default:
        return 0;
    }
    return 1;
}

extern "C" int taiko_frontend_browser_text(const char* text)
{
    std::lock_guard<std::recursive_mutex> action(g_browser_action_lock);
    if (!text || !text[0] ||
        g_phase.load(std::memory_order_acquire) != Phase::SongSelect ||
        !g_song_search_active.load(std::memory_order_acquire) ||
        g_song_launch_requested.load(std::memory_order_acquire))
        return 0;

    const unsigned selected =
        g_song_selection.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(g_song_browser_lock);
        if (g_song_browser_level != SongBrowserLevel::Songs) return 0;
        constexpr std::size_t maximum_query_bytes = 120;
        const std::string_view incoming(text);
        if (g_song_query.size() + incoming.size() <= maximum_query_bytes)
            g_song_query.append(incoming);
        rebuild_song_matches_locked(selected);
    }
    show_current_song();
    return 1;
}

extern "C" int taiko_frontend_browser_captures_text(void)
{
    std::lock_guard<std::mutex> lock(g_song_browser_lock);
    return g_phase.load(std::memory_order_acquire) == Phase::SongSelect &&
           g_song_browser_level == SongBrowserLevel::Songs &&
           g_song_search_active.load(std::memory_order_acquire) &&
           !g_song_launch_requested.load(std::memory_order_acquire);
}

extern "C" void taiko_frontend_guest_tick(ppu_context* ctx)
{
    /* PC Mode reuses this verified Player Entry dispatcher hook even when the
     * legacy TAIKO_HOST_FRONTEND overlay is disabled. */
    taiko_pc_mode_entry_tick(ctx);
    if (!enabled() || !ctx) return;
    const uint32_t controller = static_cast<uint32_t>(ctx->gpr[3]);
    if (!controller) return;
    const uint32_t state = vm_read32(controller + kStateOffset);
    Phase phase = g_phase.load(std::memory_order_acquire);

    if (phase == Phase::WaitingForEntry && state == kStateEntryMain) {
        static const bool legacy_overlay = [] {
            const char* s = std::getenv("TAIKO_OVERLAY_ENTRY");
            return s && s[0] && std::strcmp(s, "0") != 0;
        }();
        if (!legacy_overlay) return;

        g_controller = controller;
        g_toc = static_cast<uint32_t>(ctx->gpr[2]);
        g_selection.store(0, std::memory_order_release);
        for (auto& level : g_raw_levels)
            level.store(0, std::memory_order_relaxed);
        g_phase.store(Phase::LoginMenu, std::memory_order_release);
        taiko_overlay_show_entry_menu(0);
        std::fprintf(stderr,
                     "[taiko_frontend] host login owns Player Entry %08X\n",
                     controller);
        return;
    }

    if (controller != g_controller || static_cast<uint32_t>(ctx->gpr[2]) != g_toc)
        return;

    if (phase == Phase::AnonymousRequested && state == kStateEntryMain) {
        if (!invoke_player_join(controller, g_toc, true)) {
            g_phase.store(Phase::Failed, std::memory_order_release);
            taiko_overlay_clear();
            std::fprintf(stderr,
                         "[taiko_frontend] anonymous join failed; released "
                         "input to stock Player Entry\n");
            return;
        }
        g_session_label = "ANONYMOUS - OFFLINE";
        g_finish_delay = 2;
        g_finish_stage = 0;
        g_finish_timeout = 600;
        g_phase.store(Phase::Finishing, std::memory_order_release);
        taiko_overlay_show_entry_progress(g_session_label);
        return;
    }

    if (phase == Phase::BaidWaiting) {
        if (state != kStateCardSelect) {
            g_card_select_ticks = 0;
            return;
        }

        /* This hook runs before func_001EF214's native state update. On the
         * first tick that observes CardSelect, its selection model has not yet
         * received even one CardSelect update, so func_00226A9C falls back to
         * the initialized no-card record. Let that native update complete and
         * invoke the callback on the following dispatcher tick. */
        if (g_card_select_ticks++ == 0) {
            std::fprintf(stderr,
                         "[taiko_frontend] CardSelect reached; waiting for "
                         "one native update before authenticated commit\n");
            return;
        }

        {
            if (!invoke_player_join(controller, g_toc, false)) {
                release_to_stock("authenticated player join failed");
                return;
            }
            g_session_label = "BANAPASSPORT PLAYER";
            g_finish_delay = 2;
            g_finish_stage = 0;
            g_finish_timeout = 600;
            g_phase.store(Phase::Finishing, std::memory_order_release);
            taiko_overlay_show_entry_progress(g_session_label);
            std::fprintf(stderr,
                         "[taiko_frontend] authenticated P1 ready; "
                         "bypassing CardSelect Lumen\n");
        }
        return;
    }

    if (phase == Phase::Finishing) {
        if (state == kStateUserDataWait) {
            if (g_finish_stage == 1) {
                g_finish_stage = 2;
                std::fprintf(stderr,
                             "[taiko_frontend] native handoff reached state "
                             "%u; waiting for Player Entry termination\n",
                             state);
            }
        }
        /* End transitions to Term later in this same dispatcher call. There
         * is no subsequent Player Entry tick at state 40 because the scene
         * destroys the controller, so End is the last observable safe point. */
        if (state == kStateEnd || state == kStateTerm) {
            g_phase.store(Phase::SongSelect, std::memory_order_release);
            enter_song_select_shell();
            std::fprintf(stderr,
                         "[taiko_frontend] Player Entry reached final state "
                         "%u; host Song Select owns the screen\n", state);
            return;
        }
        if (g_finish_delay) {
            --g_finish_delay;
            return;
        }
        if (g_finish_timeout && --g_finish_timeout == 0) {
            std::fprintf(stderr,
                         "[taiko_frontend] Player Entry handoff timed out "
                         "in state %u\n", state);
            release_to_stock("native handoff timed out");
            return;
        }
        if (g_finish_stage == 0) {
            if (!begin_native_handoff(controller, g_toc)) {
                release_to_stock("native game-mode transaction failed");
                return;
            }
            g_finish_stage = 1;
        }
        return;
    }

    if (state == kStateTerm && phase != Phase::SongSelect) {
        g_phase.store(Phase::SongSelect, std::memory_order_release);
        enter_song_select_shell();
    }
}

extern "C" void taiko_frontend_song_select_tick(ppu_context* ctx)
{
    if ((!enabled() && !taiko_pc_mode_is_active()) || !ctx ||
        g_phase.load(std::memory_order_acquire) != Phase::SongSelect)
        return;

    const uint32_t scene = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t toc = static_cast<uint32_t>(ctx->gpr[2]);
    if (!scene || vm_read32(scene) != kGameSongSelectVtable) return;
    if (!g_song_launch_requested.load(std::memory_order_acquire)) return;

    const uint32_t state = vm_read32(scene + kSongSelectStateOffset);
    const uint32_t manager = vm_read32(scene + kSongSelectManagerOffset);
    const unsigned catalog_index =
        g_song_selection.load(std::memory_order_acquire);
    const unsigned difficulty =
        g_song_difficulty.load(std::memory_order_acquire);
    const TaikoCatalogSong* song = taiko_catalog_song(catalog_index);
    uint32_t live_index = 0;
    uint32_t live_record = 0;

    /* State 10 is stock confirmation and 11 is its post-commit state.  A host
     * request is valid only while the normal interactive machine is still
     * before those states. */
    if (!manager || !song || difficulty >= TAIKO_DIFFICULTY_COUNT ||
        !(song->difficulty_mask & (1u << difficulty)) || state >= 10 ||
        !find_live_song(manager, song->music_id, &live_index, &live_record)) {
        std::fprintf(stderr,
                     "[taiko_frontend] launch rejected scene=%08X state=%u "
                     "manager=%08X id=%s difficulty=%u\n",
                     scene, state, manager,
                     song ? song->music_id.c_str() : "(missing)", difficulty);
        g_song_launch_requested.store(false, std::memory_order_release);
        show_current_song();
        return;
    }

    const uint32_t p1_difficulty =
        scene + kSongSelectP1DifficultyOffset;
    const uint32_t p2_difficulty =
        scene + kSongSelectP2DifficultyOffset;
    vm_write32(p1_difficulty + 4, difficulty);

    /* Reproduce normal GameSongSelect state 10's semantic transaction.  The
     * argument record refers to the scene's existing player-course objects;
     * func_007FCE6C then populates Enso through Green's own managers. */
    vm_write32(kCallbackFrame + 0x00, live_index);
    vm_write32(kCallbackFrame + 0x04, p1_difficulty);
    vm_write32(kCallbackFrame + 0x08, p2_difficulty);
    vm_write32(kCallbackFrame + 0x0C,
               vm_read8(scene + kSongSelectModeFlagOffset));
    vm_write32(scene + kSongSelectNextOffset, 2);
    ppu_guest_call_ct(kSongSelectCallback, toc,
                      scene + kSongSelectCallbackOffset, 0, 0, 0);
    ppu_guest_call_ct(kMusicSelectionCommit, toc,
                      kCallbackFrame, manager, 0, 0);
    vm_write32(scene + kSongSelectStateOffset, 11);

    std::fprintf(stderr,
                 "[taiko_frontend] native Song Select commit scene=%08X "
                 "manager=%08X state=%u live_index=%u record=%08X id=%s "
                 "difficulty=%u\n",
                 scene, manager, state, live_index, live_record,
                 song->music_id.c_str(), difficulty);
    g_song_launch_requested.store(false, std::memory_order_release);
    g_phase.store(Phase::Passthrough, std::memory_order_release);
    taiko_overlay_animate_browser(1);
}

extern "C" int taiko_frontend_results_end_override(ppu_context* ctx)
{
    if (ctx && taiko_pc_mode_is_active() && taiko_pc_mode_is_standalone())
        return taiko_pc_mode_results_return(static_cast<uint32_t>(ctx->gpr[3]),
                                            static_cast<uint32_t>(ctx->gpr[4]));
    if ((!enabled() && !taiko_pc_mode_is_active()) || !ctx ||
        g_phase.load(std::memory_order_acquire) != Phase::Passthrough)
        return 0;

    const uint32_t results = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t scene_owner = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t toc = static_cast<uint32_t>(ctx->gpr[2]);
    if (!results || !scene_owner ||
        vm_read32(results) != kGameEnsoResultVtable)
        return 0;

    const uint32_t manager = vm_read32(results + 0x0C);
    if (!manager) return 0;
    const uint32_t played = vm_read32(manager + 0x408);
    const uint32_t limit = vm_read32(manager + 0x40C);

    /* Results has already reported presentation completion when its caller
     * reaches the normal session-finished routine.  Reuse the title's other
     * native destination: it copy-constructs GameSongSelect with this same
     * manager, queues it, and removes Results.  The caller then performs the
     * same common Lumen/resource cleanup used by the stock continuation path. */
    ppu_guest_call_ct(kResultsContinueToSongSelect, toc,
                      results, scene_owner, 0, 0);
    std::fprintf(stderr,
                 "[taiko_frontend] Results redirected to host Song Select "
                 "results=%08X manager=%08X played=%u limit=%u\n",
                 results, manager, played, limit);
    return 1;
}

extern "C" int taiko_frontend_results_continue_tick(ppu_context* ctx)
{
    if (ctx && taiko_pc_mode_is_active() && taiko_pc_mode_is_standalone())
        return taiko_pc_mode_results_return(static_cast<uint32_t>(ctx->gpr[3]),
                                            static_cast<uint32_t>(ctx->gpr[4]));
    if ((!enabled() && !taiko_pc_mode_is_active()) || !ctx ||
        g_phase.load(std::memory_order_acquire) != Phase::Passthrough)
        return 0;

    const uint32_t results = static_cast<uint32_t>(ctx->gpr[3]);
    if (!results || vm_read32(results) != kGameEnsoResultVtable) return 0;

    g_phase.store(Phase::SongSelect, std::memory_order_release);
    enter_song_select_shell();
    taiko_overlay_animate_browser(0);
    std::fprintf(stderr,
                 "[taiko_frontend] host Song Select reacquired after Results "
                 "results=%08X\n", results);
    return 0;
}

extern "C" void taiko_frontend_standalone_failure(const char* detail)
{
    std::lock_guard<std::recursive_mutex> action(g_browser_action_lock);
    g_browser_players.ready = 0;
    if (!taiko_pc_mode_is_active() || !taiko_pc_mode_is_standalone()) return;
    g_song_launch_requested.store(false, std::memory_order_release);
    show_current_song();
    taiko_overlay_set_browser_save_status(detail ? detail : "Song preparation failed");
    std::fprintf(stderr, "[taiko_frontend] standalone launch failed: %s\n",
                 detail ? detail : "unknown failure");
}

extern "C" void taiko_frontend_standalone_gameplay(void)
{
    g_song_launch_requested.store(false, std::memory_order_release);
    g_phase.store(Phase::Passthrough, std::memory_order_release);
    taiko_host_audio_begin_gameplay_handoff();
    taiko_overlay_animate_browser(1);
}

extern "C" void taiko_frontend_standalone_session_begin(void)
{
    std::lock_guard<std::recursive_mutex> action(g_browser_action_lock);
    g_browser_players = {};
    g_browser_accounts = {};
    for (unsigned slot = 0; slot < 2; ++slot)
        taiko_overlay_set_browser_account(slot, "", 0);
    g_player_song_index = ~0u;
}

extern "C" void taiko_frontend_browser_account(unsigned slot, const char* name, int authenticated)
{
    if (slot > 1) return;
    std::lock_guard<std::recursive_mutex> action(g_browser_action_lock);
    g_browser_accounts.players[slot] = {name ? name : "", authenticated != 0};
    taiko_overlay_set_browser_account(slot, name, authenticated);
}

extern "C" void taiko_frontend_browser_login_tick(uint32_t manager, int score_pending)
{
    // This boundary and the native response dispatcher run on the main PPU
    // thread. In particular, state 8 is written BEFORE the crown vector, so
    // observing it from an input or rendering thread would not be sufficient.
    static thread_local bool in_tick = false;
    if (in_tick) return;
    struct Guard { bool& flag; Guard(bool& f): flag(f) { flag = true; } ~Guard() { flag = false; } } guard(in_tick);
    std::lock_guard<std::recursive_mutex> action(g_browser_action_lock);
    constexpr uint32_t storage = 0xcffb0000u;
    constexpr uint32_t receiver = storage + 0x1000;
    constexpr uint32_t blank = storage + 0x1100;
    constexpr uint32_t wrapper = storage + 0x1600;
    static bool constructed = false;
    static unsigned stage = 0;
    static unsigned slot = 0;
    static uint64_t token = 0;
    static std::chrono::steady_clock::time_point deadline;
    const auto call = [](uint32_t fn, uint32_t a, uint32_t b = 0) {
        // Request OPDs use the network TOC; Entry methods use the Entry TOC.
        const uint32_t toc = (fn == 0x000a1138 || fn == 0x000a0ca4 || fn == 0x000a0998 ||
                              fn == 0x00626e30 || fn == 0x006285b8)
                                 ? 0x01027c58 : 0x01037a88;
        return static_cast<uint32_t>(ppu_guest_call_ct(fn, toc, a, b, 0, 0));
    };
    const bool browser = g_phase.load(std::memory_order_acquire) == Phase::SongSelect &&
                         !g_song_launch_requested.load(std::memory_order_acquire);
    if (!browser && !stage) {
        if (g_browser_card_lease) cancel_browser_login();
        return;
    }
    if (g_browser_accounts.phase == taiko_plus::AccountPhase::WaitingForCard &&
        taiko_card_browser_take(g_browser_card_lease, g_browser_card_code, g_browser_card_uid)) {
        taiko_card_browser_end(g_browser_card_lease);
        g_browser_card_lease = 0;
        g_browser_accounts.card_ready(g_browser_accounts.generation);
        g_browser_accounts.status = "Choose 1 for P1 or 2 for P2";
        show_browser_login();
    }
    const uint32_t owner = vm_read32(0x01033f08);
    if (!stage && g_browser_accounts.phase == taiko_plus::AccountPhase::Loading) {
        if (score_pending) {
            g_browser_accounts.status = "Waiting for previous score to save...";
            show_browser_login();
            return;
        }
        token = g_browser_accounts.generation;
        if (!manager || !owner || vm_read32(owner + 8)) {
            g_browser_accounts.fail(token, "Native login service is busy - try again");
            show_browser_login();
            return;
        }
        if (!constructed) {
            for (unsigned off = 0; off < 0x1700; off += 4) vm_write32(storage + off, 0);
            // Construct owning profiles once; assignment releases previous
            // strings/vectors. These bounded records live until process exit.
            call(0x00626e30, storage + 0x58);
            call(0x00626e30, storage + 0x548);
            call(0x00626e30, blank);
            constructed = true;
        }
        slot = static_cast<unsigned>(g_browser_accounts.destination);
        const uint32_t record = storage + 0x38 + slot * 0x4f0;
        call(0x006285b8, record + 0x20, blank);
        vm_write32(storage + 0xc, manager);
        vm_write8(record, 1);
        vm_write8(record + 1, 0);
        // Reader record uses NUL-terminated UID and access-code strings.
        char uid[9];
        std::snprintf(uid, sizeof uid, "%02X%02X%02X%02X", g_browser_card_uid[0],
                      g_browser_card_uid[1], g_browser_card_uid[2], g_browser_card_uid[3]);
        for (unsigned i = 0; i < 36; ++i) vm_write8(record + 0x3ae + i, i < 8 ? uid[i] : 0);
        for (unsigned i = 0; i < 24; ++i) vm_write8(record + 0x3d2 + i, i < 20 ? g_browser_card_code[i] : 0);
        vm_write32(record + 0x3ec, 2); // BanaPassport (native reader kind 7).
        std::memset(g_browser_card_code, 0, sizeof g_browser_card_code);
        for (unsigned off = 0; off < 0x2c; off += 4) vm_write32(receiver + off, 0);
        vm_write32(receiver + 0x14, 0xb4);
        vm_write32(receiver + 0x18, slot);
        vm_write32(receiver + 0x28, manager);
        call(0x00233820, receiver, storage);
        call(0x00233254, receiver, record);
        vm_write32(wrapper, record);
        g_browser_accounts.native_started(token);
        stage = 1;
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        g_browser_accounts.status = "Looking up BanaPassport...";
        show_browser_login();
        call(0x000a1138, wrapper);
        return;
    }
    if (!stage) return;
    // Keep the receiver bound after cancellation/timeout until the in-flight
    // response arrives. Starting another request would redirect late callbacks.
    const uint32_t state = vm_read32(receiver + 0xc);
    const uint32_t result = vm_read32(receiver + 0x1c);
    const bool terminal = state == 3 || state == 4 || state == 7 || state == 8 || result != 0;
    if (!terminal) {
        if (std::chrono::steady_clock::now() >= deadline &&
            g_browser_accounts.phase == taiko_plus::AccountPhase::Loading) {
            g_browser_accounts.fail(token, "Login timed out - waiting for request to close");
            show_browser_login();
        }
        return;
    }
    const uint32_t record = storage + 0x38 + slot * 0x4f0;
    const bool valid = browser && token == g_browser_accounts.generation &&
                      g_browser_accounts.phase == taiko_plus::AccountPhase::Loading;
    const bool success = result == 1 && vm_read8(receiver + 1) && vm_read8(record + 0x3ad);
    if (valid && success && stage < 3) {
        ++stage;
        call(0x00233254, receiver, record);
        g_browser_accounts.status = stage == 2 ? "Loading player data..." : "Loading crowns...";
        show_browser_login();
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        call(stage == 2 ? 0x000a0ca4 : 0x000a0998, wrapper);
        return;
    }
    if (valid && success && stage == 3 && state == 8 && !score_pending) {
        const uint32_t map = manager + 0x370;
        // Do not let both player slots submit results for the same identity.
        bool duplicate = false;
        const uint32_t begin = vm_read32(map), count = vm_read32(map + 4);
        if (count <= 2) for (unsigned i = 0; i < count; ++i) {
            const uint32_t other = begin + i * 0x7a8;
            if (vm_read32(other) != slot && vm_read8(other + 0x395) &&
                vm_read32(other + 0x40) == vm_read32(record + 0x58)) duplicate = true;
        }
        if (!duplicate && count <= 2) {
            vm_write32(wrapper + 4, slot);
            const uint32_t destination = call(0x005c59bc, map, wrapper + 4);
            if (destination) {
                // Native map accessor returns the value/profile, already past the slot key.
                call(0x006285b8, destination, record + 0x20);
                call(0x001edf48, storage, slot);
                const uint32_t profile = destination;
                const uint32_t length = vm_read32(profile + 0x14), capacity = vm_read32(profile + 0x18);
                const uint32_t data = capacity <= 15 ? profile + 4 : vm_read32(profile + 4);
                char name[128] = {};
                if (data && length < sizeof name && length <= capacity)
                    for (unsigned i = 0; i < length; ++i) name[i] = vm_read8(data + i);
                if (g_browser_accounts.complete(token, {name, true})) {
                    taiko_overlay_set_browser_account(slot, name, 1);
                    g_browser_players.join(slot);
                    g_browser_players.ready = 0;
                    show_current_song();
                }
            } else g_browser_accounts.fail(token, "Could not assign player");
        } else g_browser_accounts.fail(token, duplicate ? "Account is already assigned to the other player" : "Invalid player session");
    } else if (valid) g_browser_accounts.fail(token, "Unable to load account - try again");
    call(0x00233804, receiver);
    g_browser_accounts.native_finished(token);
    stage = 0;
    show_browser_login();
}
