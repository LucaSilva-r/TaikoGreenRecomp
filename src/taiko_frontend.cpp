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
#include "taiko_frontend.h"

#include "ppu_recomp.h"
#include "taiko_catalog.h"
#include "taiko_host_input.h"
#include "taiko_overlay.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" uint64_t ppu_guest_call_ct(uint32_t code, uint32_t toc,
                                        uint64_t a0, uint64_t a1,
                                        uint64_t a2, uint64_t a3);

namespace {

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
std::atomic<bool> g_song_launch_requested{false};
std::array<std::atomic<uint32_t>, 2> g_raw_levels{};
uint32_t g_controller = 0;
uint32_t g_toc = 0;
unsigned g_finish_delay = 0;
unsigned g_finish_stage = 0;
unsigned g_finish_timeout = 0;
const char* g_session_label = "P1";

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
    if (!enabled()) return false;
    const Phase phase = g_phase.load(std::memory_order_acquire);
    return phase != Phase::WaitingForEntry && phase != Phase::Passthrough &&
           phase != Phase::Failed;
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

void show_current_song()
{
    const std::size_t count = taiko_catalog_count();
    if (!count) {
        taiko_overlay_show_song_select(g_session_label);
        return;
    }
    unsigned selection = g_song_selection.load(std::memory_order_acquire);
    selection %= static_cast<unsigned>(count);
    const TaikoCatalogSong* song = taiko_catalog_song(selection);
    if (!song) return;
    unsigned difficulty = g_song_difficulty.load(std::memory_order_acquire);
    difficulty = normalize_difficulty(*song, difficulty);
    g_song_selection.store(selection, std::memory_order_release);
    g_song_difficulty.store(difficulty, std::memory_order_release);
    taiko_overlay_show_song_browser(
        g_session_label, song->music_id.c_str(), song->title.c_str(),
        song->genre.c_str(), song->unique_id, selection,
        static_cast<unsigned>(count),
        taiko_catalog_difficulty_name(difficulty));
}

void enter_song_select_shell()
{
    g_song_launch_requested.store(false, std::memory_order_release);
    g_song_selection.store(0, std::memory_order_release);
    g_song_difficulty.store(TAIKO_DIFFICULTY_ONI, std::memory_order_release);
    (void)taiko_catalog_load();
    show_current_song();
}

void handle_rising(unsigned player, uint32_t rising)
{
    if (player != 0 || !rising) return;
    const Phase phase = g_phase.load(std::memory_order_acquire);
    if (phase == Phase::SongSelect) {
        if (g_song_launch_requested.load(std::memory_order_acquire)) return;
        const std::size_t count = taiko_catalog_count();
        if (!count) return;
        unsigned selection = g_song_selection.load(std::memory_order_relaxed);
        if (rising & (TAIKO_ACTION_HIT_SL | TAIKO_ACTION_UP))
            selection = selection ? selection - 1 :
                        static_cast<unsigned>(count - 1);
        else if (rising & (TAIKO_ACTION_HIT_SR | TAIKO_ACTION_DOWN))
            selection = (selection + 1) % static_cast<unsigned>(count);
        g_song_selection.store(selection, std::memory_order_release);

        const TaikoCatalogSong* song = taiko_catalog_song(selection);
        if (song && (rising & TAIKO_ACTION_HIT_CL)) {
            const unsigned current =
                g_song_difficulty.load(std::memory_order_relaxed);
            g_song_difficulty.store(
                cycle_difficulty(*song, current, -1),
                std::memory_order_release);
        } else if (song && (rising & TAIKO_ACTION_HIT_CR)) {
            g_song_launch_requested.store(true, std::memory_order_release);
            std::fprintf(stderr,
                         "[taiko_frontend] launch requested id=%s "
                         "difficulty=%u\n",
                         song->music_id.c_str(),
                         g_song_difficulty.load(std::memory_order_relaxed));
        }
        show_current_song();
        return;
    }
    if (phase != Phase::LoginMenu) return;

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

    const uint32_t record = controller + kPlayerRecordOffset;
    const uint8_t active = vm_read8(record);
    const uint8_t kind = vm_read8(record + 0x42B);
    std::fprintf(stderr,
                 "[taiko_frontend] player join transaction mode=%s "
                 "record=%08X active=%u kind=%u\n",
                 require_offline_record ? "offline" : "authenticated",
                 record, active, kind);
    if (active != 1) return false;
    if (require_offline_record && kind != 2) return false;

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

extern "C" void taiko_frontend_guest_tick(ppu_context* ctx)
{
    if (!enabled() || !ctx) return;
    const uint32_t controller = static_cast<uint32_t>(ctx->gpr[3]);
    if (!controller) return;
    const uint32_t state = vm_read32(controller + kStateOffset);
    Phase phase = g_phase.load(std::memory_order_acquire);

    if (phase == Phase::WaitingForEntry && state == kStateEntryMain) {
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
        /* CardSelect is reached only after baidcheck, profile application and
         * costume loading have all succeeded. Invoke the title's native
         * selection callback in that prepared manager context instead of
         * waiting for its Lumen choice to set the record's active byte. */
        if (state == kStateCardSelect) {
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
    if (!enabled() || !ctx ||
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
    taiko_overlay_hide_host_screen();
}

extern "C" int taiko_frontend_results_end_override(ppu_context* ctx)
{
    if (!enabled() || !ctx ||
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

extern "C" void taiko_frontend_results_continue_tick(ppu_context* ctx)
{
    if (!enabled() || !ctx ||
        g_phase.load(std::memory_order_acquire) != Phase::Passthrough)
        return;

    const uint32_t results = static_cast<uint32_t>(ctx->gpr[3]);
    if (!results || vm_read32(results) != kGameEnsoResultVtable) return;

    g_phase.store(Phase::SongSelect, std::memory_order_release);
    enter_song_select_shell();
    std::fprintf(stderr,
                 "[taiko_frontend] host Song Select reacquired after Results "
                 "results=%08X\n", results);
}
