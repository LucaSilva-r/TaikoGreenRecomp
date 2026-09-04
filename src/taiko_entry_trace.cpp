/* State-change tracing for reconstructing Green's Player Entry and Song Select
 * controllers.  The lifted functions remain authoritative: these wrappers
 * record their guest-visible object mutations before and after forwarding to
 * the original implementation.
 *
 * Enable with TAIKO_ENTRY_TRACE=1.  Add TAIKO_ENTRY_TRACE_CALLS=1 to print
 * every wrapped call even when its object snapshot did not change.
 */

#include "ppu_recomp.h"
#include "taiko_pc_mode.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" void ppu_set_project_register_hooks(void (*register_hooks)(void));
extern "C" void ppu_set_guest_byte_watch(uint32_t begin, uint32_t end,
                                           unsigned budget);

namespace {

constexpr uint32_t kGameEntryReset = 0x000A8A3Cu;
constexpr uint32_t kGameEntryMain = 0x000AD13Cu;
constexpr uint32_t kSongSelectStateMachine = 0x000D2208u;
constexpr uint32_t kSongSelectSceneEnter = 0x000FA0C0u;
constexpr uint32_t kCardSelectInputFour = 0x00224620u;
constexpr uint32_t kCardSelectInputTwo = 0x00224864u;
constexpr uint32_t kCardSelectStatus = 0x00224C54u;

constexpr size_t kGameEntryWords = 0xA0 / sizeof(uint32_t);
constexpr size_t kSongSelectWords = 0x100 / sizeof(uint32_t);
constexpr size_t kMaximumWords = kSongSelectWords;
constexpr size_t kRecordCount = 8;
constexpr uint32_t kPlayerRecordsOffset = 0x38;
constexpr size_t kPlayerRecordSize = 0x4F0;
constexpr size_t kPlayerCount = 2;
constexpr size_t kPlayerSnapshotCount = 4;
constexpr size_t kMaximumPlayerDiffs = 96;

enum class ObjectKind : uint8_t {
    GameEntry,
    SongSelect,
};

struct SnapshotRecord {
    bool used = false;
    bool initialized = false;
    ObjectKind kind = ObjectKind::GameEntry;
    uint32_t object = 0;
    size_t words = 0;
    std::array<uint32_t, kMaximumWords> value{};
};

struct PlayerSnapshotRecord {
    bool used = false;
    bool initialized = false;
    uint32_t controller = 0;
    std::array<std::array<uint8_t, kPlayerRecordSize>, kPlayerCount> value{};
};

std::mutex g_trace_mutex;
std::array<SnapshotRecord, kRecordCount> g_records{};
std::array<PlayerSnapshotRecord, kPlayerSnapshotCount> g_player_records{};
std::atomic<uint64_t> g_trace_sequence{0};

bool setting_enabled(const char* name)
{
    const char* value = std::getenv(name);
    return value && value[0] && std::strcmp(value, "0") != 0;
}

bool trace_enabled()
{
    static const bool enabled = setting_enabled("TAIKO_ENTRY_TRACE");
    return enabled;
}

bool trace_calls_enabled()
{
    static const bool enabled = setting_enabled("TAIKO_ENTRY_TRACE_CALLS");
    return enabled;
}

const char* kind_name(ObjectKind kind)
{
    return kind == ObjectKind::GameEntry ? "GameEntry" : "SongSelect";
}

SnapshotRecord& record_for(ObjectKind kind, uint32_t object, size_t words)
{
    for (SnapshotRecord& record : g_records) {
        if (record.used && record.kind == kind && record.object == object)
            return record;
    }
    for (SnapshotRecord& record : g_records) {
        if (!record.used) {
            record.used = true;
            record.kind = kind;
            record.object = object;
            record.words = words;
            return record;
        }
    }

    /* Scene objects are short lived and there should be at most one of each.
     * Recycle the oldest slot deterministically if repeated scene entry uses
     * more addresses than the small diagnostic table can retain. */
    SnapshotRecord& record = g_records[object % g_records.size()];
    record = {};
    record.used = true;
    record.kind = kind;
    record.object = object;
    record.words = words;
    return record;
}

void read_snapshot(uint32_t object, size_t words,
                   std::array<uint32_t, kMaximumWords>& out)
{
    for (size_t i = 0; i < words; ++i)
        out[i] = vm_read32(object + i * sizeof(uint32_t));
}

uint32_t player_record_hash(const std::array<uint8_t, kPlayerRecordSize>& value)
{
    uint32_t hash = 2166136261u;
    for (uint8_t byte : value) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

PlayerSnapshotRecord& player_record_for(uint32_t controller)
{
    for (PlayerSnapshotRecord& record : g_player_records) {
        if (record.used && record.controller == controller)
            return record;
    }
    for (PlayerSnapshotRecord& record : g_player_records) {
        if (!record.used) {
            record.used = true;
            record.controller = controller;
            return record;
        }
    }

    PlayerSnapshotRecord& record =
        g_player_records[controller % g_player_records.size()];
    record = {};
    record.used = true;
    record.controller = controller;
    return record;
}

void trace_player_records_locked(const char* boundary, uint32_t controller,
                                 const ppu_context* ctx, uint32_t detail)
{
    if (!controller)
        return;

    std::array<std::array<uint8_t, kPlayerRecordSize>, kPlayerCount> current{};
    for (size_t player = 0; player < kPlayerCount; ++player) {
        const uint32_t address = controller + kPlayerRecordsOffset +
                                 player * kPlayerRecordSize;
        for (size_t offset = 0; offset < kPlayerRecordSize; ++offset)
            current[player][offset] = vm_read8(address + offset);
    }

    PlayerSnapshotRecord& record = player_record_for(controller);
    if (!record.initialized) {
        std::fprintf(stderr,
                     "[entry-player] %s first tid=%llu object=%08X detail=%u "
                     "p1-active=%u p1-hash=%08X p2-active=%u p2-hash=%08X\n",
                     boundary,
                     static_cast<unsigned long long>(ctx->thread_id),
                     controller, detail, current[0][0],
                     player_record_hash(current[0]), current[1][0],
                     player_record_hash(current[1]));
    } else {
        for (size_t player = 0; player < kPlayerCount; ++player) {
            size_t changed = 0;
            for (size_t offset = 0; offset < kPlayerRecordSize; ++offset)
                changed += record.value[player][offset] != current[player][offset];
            if (!changed)
                continue;

            std::fprintf(stderr,
                         "[entry-player] %s tid=%llu object=%08X detail=%u "
                         "p%zu changed=%zu active=%u->%u hash=%08X->%08X\n",
                         boundary,
                         static_cast<unsigned long long>(ctx->thread_id),
                         controller, detail, player + 1, changed,
                         record.value[player][0], current[player][0],
                         player_record_hash(record.value[player]),
                         player_record_hash(current[player]));

            size_t printed = 0;
            for (size_t offset = 0;
                 offset < kPlayerRecordSize && printed < kMaximumPlayerDiffs;
                 ++offset) {
                if (record.value[player][offset] == current[player][offset])
                    continue;
                if ((printed % 8) == 0)
                    std::fprintf(stderr, "[entry-player-diff p%zu]", player + 1);
                std::fprintf(stderr, " +%03zX:%02X->%02X", offset,
                             record.value[player][offset], current[player][offset]);
                if ((++printed % 8) == 0)
                    std::fputc('\n', stderr);
            }
            if ((printed % 8) != 0)
                std::fputc('\n', stderr);
            if (changed > printed) {
                std::fprintf(stderr,
                             "[entry-player-diff p%zu] ... %zu more byte changes\n",
                             player + 1, changed - printed);
            }
        }
    }

    record.value = current;
    record.initialized = true;
}

void trace_player_records(const char* boundary, uint32_t controller,
                          const ppu_context* ctx, uint32_t detail)
{
    if (!trace_enabled() || !controller)
        return;
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    trace_player_records_locked(boundary, controller, ctx, detail);
}

void print_initial(uint64_t sequence, const char* method, const char* phase,
                   ObjectKind kind, uint32_t object, size_t words,
                   const std::array<uint32_t, kMaximumWords>& value,
                   const ppu_context* ctx,
                   const std::array<uint32_t, 5>& arguments)
{
    std::fprintf(stderr,
                 "[entry-trace %llu] %s %s %s first object=%08X words=%zu "
                 "tid=%llu in-r3-r7=%08X,%08X,%08X,%08X,%08X now-r3=%08X\n",
                 static_cast<unsigned long long>(sequence), method, phase,
                 kind_name(kind), object, words,
                 static_cast<unsigned long long>(ctx->thread_id),
                 arguments[0], arguments[1], arguments[2], arguments[3],
                 arguments[4], static_cast<uint32_t>(ctx->gpr[3]));
    for (size_t base = 0; base < words; base += 8) {
        std::fprintf(stderr, "[entry-object %08X]", object);
        const size_t end = base + 8 < words ? base + 8 : words;
        for (size_t i = base; i < end; ++i)
            std::fprintf(stderr, " +%02zX=%08X", i * sizeof(uint32_t), value[i]);
        std::fputc('\n', stderr);
    }
}

size_t print_changes(uint64_t sequence, const char* method, const char* phase,
                     uint32_t object, size_t words,
                     const std::array<uint32_t, kMaximumWords>& previous,
                     const std::array<uint32_t, kMaximumWords>& current,
                     const ppu_context* ctx,
                     const std::array<uint32_t, 5>& arguments)
{
    size_t changed = 0;
    for (size_t i = 0; i < words; ++i)
        changed += previous[i] != current[i];
    if (!changed)
        return 0;

    std::fprintf(stderr,
                 "[entry-trace %llu] %s %s object=%08X changed=%zu tid=%llu "
                 "in-r3-r7=%08X,%08X,%08X,%08X,%08X now-r3=%08X\n",
                 static_cast<unsigned long long>(sequence), method, phase,
                 object, changed,
                 static_cast<unsigned long long>(ctx->thread_id),
                 arguments[0], arguments[1], arguments[2], arguments[3],
                 arguments[4], static_cast<uint32_t>(ctx->gpr[3]));
    size_t on_line = 0;
    for (size_t i = 0; i < words; ++i) {
        if (previous[i] == current[i])
            continue;
        if (on_line == 0)
            std::fprintf(stderr, "[entry-diff %08X]", object);
        std::fprintf(stderr, " +%02zX:%08X->%08X",
                     i * sizeof(uint32_t), previous[i], current[i]);
        if (++on_line == 6) {
            std::fputc('\n', stderr);
            on_line = 0;
        }
    }
    if (on_line)
        std::fputc('\n', stderr);
    return changed;
}

void trace_object(const char* method, const char* phase, ObjectKind kind,
                  uint32_t object, size_t words, const ppu_context* ctx,
                  const std::array<uint32_t, 5>& arguments)
{
    if (!trace_enabled() || !object)
        return;

    const uint64_t sequence =
        g_trace_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    std::array<uint32_t, kMaximumWords> current{};
    read_snapshot(object, words, current);
    SnapshotRecord& record = record_for(kind, object, words);
    const bool first = !record.initialized;

    if (first) {
        print_initial(sequence, method, phase, kind, object, words, current,
                      ctx, arguments);
    } else {
        const size_t changed = print_changes(sequence, method, phase, object,
                                             words, record.value, current,
                                             ctx, arguments);
        if (!changed && trace_calls_enabled()) {
            std::fprintf(stderr,
                         "[entry-trace %llu] %s %s object=%08X unchanged "
                         "tid=%llu r3-r7=%08X,%08X,%08X,%08X,%08X\n",
                         static_cast<unsigned long long>(sequence), method,
                         phase, object,
                         static_cast<unsigned long long>(ctx->thread_id),
                         arguments[0], arguments[1], arguments[2],
                         arguments[3], arguments[4]);
        }
    }
    record.value = current;
    record.initialized = true;
}

using GuestFunction = void (*)(ppu_context*);

void trace_callback(ppu_context* ctx, const char* method,
                    GuestFunction original)
{
    const uint32_t frame = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t auxiliary = frame ? vm_read32(frame + 0x10) : 0;
    const uint32_t current = frame ? vm_read32(frame + 0x1C) : 0;
    const uint32_t count = auxiliary ? vm_read32(auxiliary + 0x28) : 0;
    std::fprintf(stderr,
                 "[entry-card-callback] %s tid=%llu lr=%08X frame=%08X "
                 "current=%08X count=%u",
                 method, static_cast<unsigned long long>(ctx->thread_id),
                 static_cast<uint32_t>(ctx->lr), frame, current, count);
    const uint32_t printable = count < 8 ? count : 8;
    for (uint32_t index = 0; index < printable; ++index) {
        const uint32_t argument = current - index * 8;
        std::fprintf(stderr, " arg%u={type=%u,value=%08X}", index + 1,
                     vm_read32(argument), vm_read32(argument + 4));
    }
    std::fputc('\n', stderr);
    original(ctx);
}

void trace_call(ppu_context* ctx, const char* method, ObjectKind kind,
                size_t words, GuestFunction original)
{
    const uint32_t object = static_cast<uint32_t>(ctx->gpr[3]);
    const std::array<uint32_t, 5> arguments = {
        static_cast<uint32_t>(ctx->gpr[3]),
        static_cast<uint32_t>(ctx->gpr[4]),
        static_cast<uint32_t>(ctx->gpr[5]),
        static_cast<uint32_t>(ctx->gpr[6]),
        static_cast<uint32_t>(ctx->gpr[7]),
    };
    trace_object(method, "enter", kind, object, words, ctx, arguments);
    original(ctx);
    trace_object(method, "leave", kind, object, words, ctx, arguments);
}

void trace_game_entry_reset(ppu_context* ctx)
{
    trace_call(ctx, "func_000A8A3C", ObjectKind::GameEntry,
               kGameEntryWords, func_000A8A3C);
}

void trace_game_entry_main(ppu_context* ctx)
{
    trace_call(ctx, "func_000AD13C", ObjectKind::GameEntry,
               kGameEntryWords, func_000AD13C);
}

void trace_song_select_state(ppu_context* ctx)
{
    trace_call(ctx, "func_000D2208", ObjectKind::SongSelect,
               kSongSelectWords, func_000D2208);
}

void trace_song_select_enter(ppu_context* ctx)
{
    trace_call(ctx, "func_000FA0C0", ObjectKind::SongSelect,
               kSongSelectWords, func_000FA0C0);
}

void trace_card_select_input_four(ppu_context* ctx)
{
    trace_callback(ctx, "func_00224620", func_00224620);
}

void trace_card_select_input_two(ppu_context* ctx)
{
    trace_callback(ctx, "func_00224864", func_00224864);
}

void trace_card_select_status(ppu_context* ctx)
{
    trace_callback(ctx, "func_00224C54", func_00224C54);
}

void register_taiko_entry_trace_hooks()
{
    if (!trace_enabled())
        return;
    ppu_register_function(kGameEntryReset, trace_game_entry_reset);
    ppu_register_function(kGameEntryMain, trace_game_entry_main);
    ppu_register_function(kSongSelectStateMachine, trace_song_select_state);
    ppu_register_function(kSongSelectSceneEnter, trace_song_select_enter);
    ppu_register_function(kCardSelectInputFour, trace_card_select_input_four);
    ppu_register_function(kCardSelectInputTwo, trace_card_select_input_two);
    ppu_register_function(kCardSelectStatus, trace_card_select_status);
    std::fprintf(stderr,
                 "[entry-trace] installed GameEntry/SongSelect/CardSelect "
                 "wrappers; "
                 "all-calls=%u\n",
                 trace_calls_enabled() ? 1u : 0u);
}

__attribute__((constructor))
void configure_taiko_entry_trace_hooks()
{
    ppu_set_project_register_hooks(register_taiko_entry_trace_hooks);
}

} // namespace

/* Verified Player Entry event boundary.  A temporary diagnostic line at the
 * start of lifted func_001ED74C preserves the incoming event ID and, crucially,
 * the return address of the producer before the lifted prologue saves it. */
extern "C" void taiko_entry_event_trace(ppu_context* ctx)
{
    if (!ctx || !trace_enabled())
        return;

    const uint32_t object = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t event = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t current = object ? vm_read32(object + 0x14) : UINT32_MAX;
    const uint32_t previous = object ? vm_read32(object + 0x10) : UINT32_MAX;
    trace_player_records("event", object, ctx, event);
    std::fprintf(stderr,
                 "[entry-event] tid=%llu lr=%08X object=%08X "
                 "event=%u state=%u previous=%u\n",
                 static_cast<unsigned long long>(ctx->thread_id),
                 static_cast<uint32_t>(ctx->lr), object, event, current,
                 previous);
}

/* Game-mode confirmation callback.  This is the native transaction that
 * appends a 12-byte selection record to the vector at player-record +0x4C0.
 * Its argument ABI is the same Lumen callback frame used by the no-card join,
 * but the argument count and values are different. */
extern "C" void taiko_entry_game_mode_callback_trace(ppu_context* ctx)
{
    if (!ctx)
        return;

    const uint32_t frame = static_cast<uint32_t>(ctx->gpr[3]);
    if (frame) {
        const uint32_t auxiliary = vm_read32(frame + 0x10);
        const uint32_t current = vm_read32(frame + 0x1C);
        const uint32_t count = auxiliary ? vm_read32(auxiliary + 0x28) : 0;
        if (count >= 1 && current) {
            const uint32_t game_mode = vm_read32(current + 4);
            taiko_pc_mode_on_game_mode_selected(game_mode);
            if (game_mode == TAIKO_PC_MODE_SENTINEL) {
                /* 99 is private to the added Lumen item. Let the stock
                 * callback complete its normal Play bookkeeping while the
                 * pending flag redirects the outgoing sequence task later. */
                vm_write32(current + 4, TAIKO_PC_MODE_SAFE_GAME_MODE);
                std::fprintf(stderr,
                             "[taiko_pc_mode] rewrote guest sentinel %u to "
                             "safe stock mode %u\n",
                             game_mode, TAIKO_PC_MODE_SAFE_GAME_MODE);
            }
        }
    }

    if (!trace_enabled())
        return;

    std::fprintf(stderr,
                 "[entry-game-mode-callback] tid=%llu lr=%08X frame=%08X "
                 "toc=%08X\n",
                 static_cast<unsigned long long>(ctx->thread_id),
                 static_cast<uint32_t>(ctx->lr), frame,
                 static_cast<uint32_t>(ctx->gpr[2]));
    if (!frame)
        return;

    for (uint32_t base = 0; base < 0x40; base += 0x20) {
        std::fprintf(stderr, "[entry-game-mode-frame %08X]", frame);
        for (uint32_t offset = base; offset < base + 0x20; offset += 4)
            std::fprintf(stderr, " +%02X=%08X", offset,
                         vm_read32(frame + offset));
        std::fputc('\n', stderr);
    }

    const uint32_t auxiliary = vm_read32(frame + 0x10);
    const uint32_t current = vm_read32(frame + 0x1C);
    const uint32_t count = auxiliary ? vm_read32(auxiliary + 0x28) : 0;
    std::fprintf(stderr,
                 "[entry-game-mode-args] auxiliary=%08X current=%08X "
                 "count=%u",
                 auxiliary, current, count);
    const uint32_t printable = count < 8 ? count : 8;
    for (uint32_t index = 0; index < printable; ++index) {
        const uint32_t argument = current - index * 8;
        std::fprintf(stderr, " arg%u={type=%u,value=%08X}", index + 1,
                     vm_read32(argument), vm_read32(argument + 4));
    }
    std::fputc('\n', stderr);
}

/* Verified Player Entry transition boundary.  This is called by a temporary
 * diagnostic line at the start of lifted func_001ED698.  Keeping the name
 * conversion native avoids perturbing the guest string/logging machinery. */
extern "C" void taiko_entry_state_transition_trace(ppu_context* ctx)
{
    if (!ctx || !trace_enabled())
        return;

    static constexpr const char* kStateNames[41] = {
        "None", "Init", "Load", "State_ReadySound", "State_Intermission",
        "Start", "EntryMain", "CardSelect", "Mobile",
        "OnSendURL_WaitMobile", "OnSendURL_Mismatch", "OnSendURL_Response",
        nullptr, nullptr, nullptr,
        "BAID_WaitServer", "BAID_Canceled", "BAID_Failed", "BAID_Succeed",
        nullptr, "BAID_WaitNewCostume", "MydonName_WaitServer",
        "MydonName_End", "UserData_WaitServer", "State_UserData_Failed",
        nullptr, nullptr, nullptr, nullptr, nullptr, "State_UserData_End",
        "WaitUserResponce_Succeed", "WaitUserResponce_Failed", "OnTouchCard",
        "WaitEntrybase", "WaitEndInterval", nullptr, nullptr, nullptr,
        "End", "Term",
    };

    const uint32_t object = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t requested = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t current = object ? vm_read32(object + 0x14) : UINT32_MAX;
    if (setting_enabled("TAIKO_ENTRY_TRACE_WRITES")) {
        if (requested == 6 && object) {
            const uint32_t begin = object + kPlayerRecordsOffset;
            ppu_set_guest_byte_watch(
                begin, begin + kPlayerCount * kPlayerRecordSize, 96);
        } else if (requested == 40) {
            ppu_set_guest_byte_watch(0, 0, 0);
        }
    }
    trace_player_records("state", object, ctx, requested);
    const char* old_name = current < std::size(kStateNames)
                               ? kStateNames[current] : nullptr;
    const char* new_name = requested < std::size(kStateNames)
                               ? kStateNames[requested] : nullptr;
    std::fprintf(stderr,
                 "[entry-state] tid=%llu lr=%08X object=%08X "
                 "state=%u(%s)->%u(%s) previous=%u\n",
                 static_cast<unsigned long long>(ctx->thread_id),
                 static_cast<uint32_t>(ctx->lr), object, current,
                 old_name ? old_name : "?", requested,
                 new_name ? new_name : "?",
                 object ? vm_read32(object + 0x10) : UINT32_MAX);
}

/* Temporary direct-call probe used while reconstructing the controller.  The
 * generated lift calls several Game Entry methods without going through the
 * runtime function registry, so an investigation build inserts this at those
 * function entries.  Keep the implementation here; the generated call sites
 * themselves are deliberately not part of recomp_hand_edits.json. */
extern "C" void taiko_entry_trace_direct(ppu_context* ctx, uint32_t address,
                                          uint32_t song_select)
{
    if (!ctx || !trace_enabled())
        return;

    const char* method = "unknown";
    switch (address) {
    case 0x000A7354u: method = "func_000A7354"; break;
    case 0x000A7C0Cu: method = "func_000A7C0C"; break;
    case 0x000A8654u: method = "func_000A8654"; break;
    case 0x000A8A3Cu: method = "func_000A8A3C"; break;
    case 0x000A9504u: method = "func_000A9504"; break;
    case 0x000AA5ACu: method = "func_000AA5AC"; break;
    case 0x000AD13Cu: method = "func_000AD13C"; break;
    case 0x000D2208u: method = "func_000D2208"; break;
    case 0x000FA0C0u: method = "func_000FA0C0"; break;
    }

    const ObjectKind kind = song_select ? ObjectKind::SongSelect
                                        : ObjectKind::GameEntry;
    const size_t words = song_select ? kSongSelectWords : kGameEntryWords;
    const uint32_t object = static_cast<uint32_t>(ctx->gpr[3]);
    const std::array<uint32_t, 5> arguments = {
        static_cast<uint32_t>(ctx->gpr[3]),
        static_cast<uint32_t>(ctx->gpr[4]),
        static_cast<uint32_t>(ctx->gpr[5]),
        static_cast<uint32_t>(ctx->gpr[6]),
        static_cast<uint32_t>(ctx->gpr[7]),
    };
    trace_object(method, "direct", kind, object, words, ctx, arguments);
}
