#include "taiko_pc_mode.h"
#include "taiko_frontend.h"
#include "ppu_recomp.h"
#include "taiko_overlay.h"
#include "taiko_plus_runtime.h"
#include "taiko_host_audio.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

extern "C" uint64_t ppu_guest_call_ct(uint32_t code, uint32_t toc,
                                        uint64_t a0, uint64_t a1,
                                        uint64_t a2, uint64_t a3);

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" void ppu_set_project_register_hooks(void (*register_hooks)(void));

namespace {

constexpr uint32_t kSequencePushTaskAddr = 0x008DA500u;
constexpr uint32_t kSequenceUpdateAddr = 0x0026D530u;
constexpr uint32_t kEntryStateOffset = 0x14u;
constexpr uint32_t kEntryStateEnd = 39u;
constexpr uint32_t kEntryStateTerm = 40u;

std::atomic<bool> s_pc_mode_active{false};
std::atomic<bool> s_pending_pc_mode{false};
std::atomic<bool> s_entry_handoff_armed{false};
uint32_t s_lifetime_probe_manager = 0;
uint64_t s_lifetime_probe_ticks = 0;
uint32_t s_sequence_runtime = 0;
uint32_t s_setup_anchor = 0;

bool lifetime_trace_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("TAIKO_PLUS_LIFETIME_TRACE");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

void trace_manager(const char* point)
{
    if (!lifetime_trace_enabled() || !s_lifetime_probe_manager) return;
    const uint32_t manager = s_lifetime_probe_manager;
    std::fprintf(stderr,
        "[taiko_plus_lifetime] %s manager=%08X head=%08X/%08X "
        "played=%u limit=%u songs=%08X..%08X%s\n",
        point, manager, vm_read32(manager), vm_read32(manager + 4u),
        vm_read32(manager + 0x408u), vm_read32(manager + 0x40cu),
        vm_read32(manager + 0x434u), vm_read32(manager + 0x438u),
        (vm_read32(manager) & 0xffffff00u) == 0xdddddd00u
            ? " POISONED" : "");
}

// POD call arguments only; native constructors own every player/scene object.
constexpr uint32_t kScratch = 0xcffc0000u;
constexpr uint32_t kToc = 0x01037a88u;
uint64_t s_launch_generation = 0;
unsigned s_preload_frames = 0;
unsigned s_ready_frames = 0;
bool s_transition_started = false;
bool s_preloading = false;

uint32_t native(uint32_t code, uint32_t a = 0, uint32_t b = 0,
                uint32_t c = 0)
{
    return static_cast<uint32_t>(ppu_guest_call_ct(code, kToc, a, b, c, 0));
}

bool live_song(uint32_t manager, const std::string& id, uint32_t& index)
{
    const uint32_t begin = vm_read32(manager + 0x434);
    const uint32_t end = vm_read32(manager + 0x438);
    if (!begin || end < begin || (end - begin) % 0x90 ||
        (end - begin) / 0x90 > 2048) return false;
    for (index = 0; index < (end - begin) / 0x90; ++index) {
        const uint32_t str = begin + index * 0x90 + 4;
        if (vm_read32(str + 0x10) != id.size() || id.size() > 64) continue;
        const uint32_t data = vm_read32(str + 0x14) <= 15 ? str : vm_read32(str);
        if (!data) continue;
        bool same = true;
        for (unsigned i = 0; i < id.size(); ++i)
            if (vm_read8(data + i) != static_cast<uint8_t>(id[i])) same = false;
        if (same) return true;
    }
    return false;
}

void prepare_match(const taiko_plus::MatchConfig& match)
{
    auto& runtime = taiko_plus::runtime();
    const uint32_t manager = s_lifetime_probe_manager;
    uint32_t index = 0;
    if (!manager || manager != s_sequence_runtime + 0xd8 ||
        !live_song(manager, match.content.music_id, index)) {
        runtime.fail(match.generation, taiko_plus::GuestErrorCode::InvalidContent,
                     "selected song is absent from the native session catalog");
        return;
    }
    if (!match.players[1].anonymous) {
        runtime.fail(match.generation, taiko_plus::GuestErrorCode::PlayerUnavailable,
                     "remote profile import is not implemented; use a guest P2");
        return;
    }
    // operator[] constructs default native user data and stats on insertion.
    // Inserting P2 can move P1, so acquire both pointers after insertion.
    const uint32_t map = manager + 0x370;
    const uint32_t old_count = vm_read32(map + 4);
    if (!vm_read32(map) || !old_count || old_count > 2 ||
        vm_read32(vm_read32(map)) != 0) {
        runtime.fail(match.generation, taiko_plus::GuestErrorCode::PlayerUnavailable,
                     "native Player Entry did not populate P1");
        return;
    }
    vm_write32(kScratch, 1);
    native(0x005c59bc, map, kScratch);
    vm_write32(kScratch, 0);
    const uint32_t p1 = native(0x005c59bc, map, kScratch);
    vm_write32(kScratch, 1);
    const uint32_t p2 = native(0x005c59bc, map, kScratch);
    if (!p1 || !p2) {
        runtime.fail(match.generation, taiko_plus::GuestErrorCode::PlayerUnavailable,
                     "native player construction failed");
        return;
    }
    if (vm_read32(map + 4) != old_count)
        ppu_guest_call_ct(0x00717aec, 0x01027c58, manager + 0x430, p2, 0, 0);
    vm_write32(manager + 0x400, 3); // Native participation mask: P1 and P2.
    for (uint32_t offset = 0; offset < 0x90; offset += 4)
        vm_write32(kScratch + offset, 0);
    for (unsigned slot = 0; slot < 2; ++slot) {
        const uint32_t course = kScratch + 0x10 + slot * 0x30;
        vm_write8(course, 1);
        vm_write32(course + 4, match.content.difficulty);
        vm_write32(course + 0x28, slot ? p2 : p1);
        vm_write32(course + 0x2c, (slot ? p2 : p1) + 0x4d0);
    }
    vm_write32(kScratch, index);
    vm_write32(kScratch + 4, kScratch + 0x10);
    vm_write32(kScratch + 8, kScratch + 0x40);
    native(0x007fce6c, kScratch, manager);
    s_launch_generation = match.generation;
    s_preload_frames = 0;
    s_ready_frames = 0;
    s_transition_started = false;
    s_preloading = runtime.transition(match.generation,
        taiko_plus::State::PreparingMatch, taiko_plus::State::LaunchingGameplay,
        taiko_plus::EventKind::MatchAccepted);
    std::fprintf(stderr, "[taiko_plus] native selection committed id=%s index=%u "
        "difficulty=%u players=%08X/%08X generation=%llu\n",
        match.content.music_id.c_str(), index, match.content.difficulty, p1, p2,
        static_cast<unsigned long long>(match.generation));
}

void advance_launch(uint32_t owner)
{
    if (!s_preloading) return;
    auto& runtime = taiko_plus::runtime();
    if (runtime.generation() != s_launch_generation ||
        runtime.state() != taiko_plus::State::LaunchingGameplay) {
        s_preloading = false;
        return;
    }
    const uint32_t manager = s_lifetime_probe_manager;
    ++s_preload_frames;
    if (!s_transition_started) {
        const uint32_t service = native(0x005c5c1c, manager);
        vm_write32(kScratch, 0);
        // This starts the shared Lumen transition; it is not a pure readiness
        // predicate. Once accepted, native Song Select changes state and never
        // calls it again during the following 120-frame interval.
        s_transition_started = service && static_cast<uint8_t>(
            native(0x005c583c, service, kScratch));
        if (!s_transition_started) {
            if (s_preload_frames % 300 == 0)
                std::fprintf(stderr, "[taiko_plus] waiting for native transition "
                    "frames=%u service=%08X state=%d\n", s_preload_frames,
                    service, service ? static_cast<int32_t>(vm_read32(service + 8)) : -1);
            return;
        }
        std::fprintf(stderr, "[taiko_plus] native transition accepted; waiting 120 frames\n");
        return;
    }
    if (++s_ready_frames <= 120) return;
    s_preloading = false;
    const uint32_t sound = native(0x005c544c, manager);
    if (sound) native(0x005c535c, sound, 0);
    const uint32_t scene = native(0x0035d1a0, 0x178);
    if (!scene) {
        runtime.fail(s_launch_generation,
            taiko_plus::GuestErrorCode::PartialConstructionFailed,
            "native GameEnso allocation failed");
        return;
    }
    native(0x001e1c04, scene, manager);
    // Match the native factory: queue before configuring; the current frame
    // transaction invokes initialization only after this hook returns.
    native(0x008da500, owner, scene, 0);
    vm_write32(kScratch, manager);
    vm_write32(kScratch + 0x10, 0);
    for (unsigned off = 4; off <= 0x24; off += 4)
        vm_write32(kScratch + 0x10 + off, 0xffffffff);
    vm_write32(kScratch + 0x38, 0);
    native(0x00251c08, kScratch, kScratch + 0x10);
    native(0x001de520, scene, kScratch + 0x10);
    runtime.transition(s_launch_generation, taiko_plus::State::LaunchingGameplay,
                       taiko_plus::State::Gameplay, taiko_plus::EventKind::GameplayStarted);
    taiko_frontend_standalone_gameplay();
    std::fprintf(stderr, "[taiko_plus] GameEnso queued scene=%08X state=%u owner=%08X\n",
                 scene, vm_read32(scene + 8), owner);
}

} // namespace

int taiko_pc_mode_is_active(void)
{
    return s_pc_mode_active.load(std::memory_order_acquire) ? 1 : 0;
}

int taiko_pc_mode_is_standalone(void)
{
    return taiko_plus::standalone_enabled() ? 1 : 0;
}

void taiko_pc_mode_on_game_mode_selected(uint32_t mode)
{
    if (mode == TAIKO_PC_MODE_SENTINEL) {
        std::fprintf(stderr,
                     "[taiko_pc_mode] PC Mode selected in the stock carousel\n");
        s_entry_handoff_armed.store(false, std::memory_order_release);
        s_pending_pc_mode.store(true, std::memory_order_release);
    } else {
        s_pending_pc_mode.store(false, std::memory_order_release);
        s_entry_handoff_armed.store(false, std::memory_order_release);
        std::fprintf(stderr,
                     "[taiko_pc_mode] stock arcade mode %u selected\n", mode);
    }
}

void taiko_pc_mode_entry_tick(ppu_context* ctx)
{
    if (!ctx || !s_pending_pc_mode.load(std::memory_order_acquire) ||
        s_entry_handoff_armed.load(std::memory_order_acquire))
        return;

    const uint32_t entry = static_cast<uint32_t>(ctx->gpr[3]);
    if (!entry) return;
    const uint32_t state = vm_read32(entry + kEntryStateOffset);
    if (state != kEntryStateEnd && state != kEntryStateTerm) return;

    s_lifetime_probe_manager = vm_read32(entry + 0x0cu);
    s_lifetime_probe_ticks = 0;
    trace_manager("entry-final-state");
    s_entry_handoff_armed.store(true, std::memory_order_release);
    std::fprintf(stderr,
                 "[taiko_pc_mode] Player Entry reached final state %u; "
                 "handoff armed\n", state);
}

void taiko_pc_mode_activate(uint32_t controller)
{
    s_pc_mode_active.store(true, std::memory_order_release);
    taiko_plus::runtime().activate();
    if (taiko_pc_mode_is_standalone())
        taiko_host_audio_set_scene_active(true);
    std::fprintf(stderr,
                 "[taiko_pc_mode] host PC Mode activated (%s); "
                 "SequenceController=%08X\n",
                 taiko_pc_mode_is_standalone() ? "standalone" : "legacy",
                 controller);

    taiko_overlay_clear();
    taiko_frontend_enter_song_select_shell();
}

int taiko_pc_mode_destination_override(ppu_context* ctx)
{
    if (!ctx || !taiko_pc_mode_is_standalone() ||
        !s_pending_pc_mode.load(std::memory_order_acquire) ||
        !s_entry_handoff_armed.load(std::memory_order_acquire))
        return 0;

    const uint32_t outgoing = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t scene_owner = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t vtable = scene_owner ? vm_read32(scene_owner) : 0;
    if (!outgoing || !scene_owner || !vtable) return 0;

    /* This is diagnostic observation, not ownership. The manager remains
     * unusable by standalone bootstrap until its native retain/release
     * operation has been proved. */
    s_lifetime_probe_manager = vm_read32(outgoing + 0x0cu);
    s_lifetime_probe_ticks = 0;
    trace_manager("before-entry-removal");

    const uint32_t contains_opd = vm_read32(vtable + 0x14u);
    if (!contains_opd || !vm_read32(contains_opd) ||
        !vm_read32(contains_opd + 4u))
        return 0;

    /* Mirror the exact pre-allocation prefix of func_001FE470. The +0x14
     * membership predicate returns a byte; when it is nonzero the native
     * factory invokes the +0x0c removal operation once, then proceeds to
     * construct the destination. */
    const bool contains_outgoing = static_cast<uint8_t>(ppu_guest_call_ct(
        vm_read32(contains_opd), vm_read32(contains_opd + 4u),
        scene_owner, outgoing, 0, 0)) != 0;
    if (contains_outgoing) {
        const uint32_t remove_opd = vm_read32(vtable + 0x0cu);
        if (!remove_opd || !vm_read32(remove_opd) ||
            !vm_read32(remove_opd + 4u))
            return 0;
        ppu_guest_call_ct(vm_read32(remove_opd), vm_read32(remove_opd + 4u),
                          scene_owner, outgoing, 0, 0);
        trace_manager("after-entry-removal");
    }

    s_entry_handoff_armed.store(false, std::memory_order_release);
    s_pending_pc_mode.store(false, std::memory_order_release);
    std::fprintf(stderr,
                 "[taiko_pc_mode] suppressed Song Select destination before "
                 "allocation owner=%08X outgoing=%08X\n",
                 scene_owner, outgoing);
    taiko_pc_mode_activate(scene_owner);
    return 1;
}

void taiko_pc_mode_deactivate(void)
{
    taiko_plus::runtime().deactivate();
    taiko_host_audio_set_scene_active(false);
    s_pc_mode_active.store(false, std::memory_order_release);
    s_pending_pc_mode.store(false, std::memory_order_release);
    s_entry_handoff_armed.store(false, std::memory_order_release);
    s_lifetime_probe_manager = 0;
    s_lifetime_probe_ticks = 0;
    s_setup_anchor = 0;
    s_preloading = false;
    std::fprintf(stderr, "[taiko_pc_mode] PC Mode deactivated\n");
}

int taiko_pc_mode_setup_complete(uint32_t setup, uint32_t owner)
{
    if (!taiko_pc_mode_is_standalone() ||
        !s_pending_pc_mode.load(std::memory_order_acquire) ||
        !s_entry_handoff_armed.load(std::memory_order_acquire) ||
        !setup || vm_read32(setup) != 0x00f8bae8u ||
        !owner || vm_read32(owner) != 0x00f9ae70u)
        return 0;
    s_setup_anchor = setup;
    s_lifetime_probe_manager = vm_read32(setup + 4u);
    trace_manager("song-setup-complete");
    s_entry_handoff_armed.store(false, std::memory_order_release);
    s_pending_pc_mode.store(false, std::memory_order_release);
    std::fprintf(stderr,
        "[taiko_pc_mode] native GameSongSetup complete; "
        "suppressed Song Select before allocation manager=%08X\n",
        s_lifetime_probe_manager);
    taiko_pc_mode_activate(owner);
    return 1;
}

void taiko_pc_mode_push_task_hook(ppu_context* ctx)
{
    if (!ctx) return;
    const uint32_t controller = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t task = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t task_vtable = task ? vm_read32(task) : 0;

    /* Entry first queues GameSongSetup (f8bae8), which owns the session
     * population work. It is not a Song Select variant. Keep the marker armed
     * through setup and unrelated task pushes until the destination factory. */
    if (s_entry_handoff_armed.load(std::memory_order_acquire) &&
        task_vtable != 0x00f92fc0u) {
        if (task_vtable == 0x00f8bae8u)
            std::fprintf(stderr,
                "[taiko_pc_mode] retaining native GameSongSetup task=%08X\n",
                task);
        func_008DA500(ctx);
        return;
    }

    if (s_entry_handoff_armed.exchange(false, std::memory_order_acq_rel)) {
        s_pending_pc_mode.store(false, std::memory_order_release);
        if (taiko_pc_mode_is_standalone()) {
            std::fprintf(stderr,
                         "[taiko_pc_mode] suppressing post-Entry arcade task "
                         "controller=%08X task=%08X vtable=%08X\n",
                         controller, task, task_vtable);
            taiko_pc_mode_activate(controller);
            ctx->gpr[3] = 1;
            return;
        }

        std::fprintf(stderr,
                     "[taiko_pc_mode] legacy path retains post-Entry task "
                     "controller=%08X task=%08X vtable=%08X\n",
                     controller, task, task_vtable);
        func_008DA500(ctx);
        taiko_pc_mode_activate(controller);
        return;
    }

    func_008DA500(ctx);
}

void taiko_pc_mode_frame_begin(ppu_context* ctx)
{
    if (!ctx) return;
    s_sequence_runtime = static_cast<uint32_t>(ctx->gpr[3]);
    if (!taiko_pc_mode_is_active() || !taiko_pc_mode_is_standalone()) return;
    if (taiko_plus::runtime().state() == taiko_plus::State::Gameplay) {
        const uint32_t tasks = vm_read32(s_sequence_runtime + 0x68);
        const uint32_t count = vm_read32(s_sequence_runtime + 0x6c);
        if (tasks && count <= 256) {
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t task = vm_read32(tasks + i * 4);
                if (task && vm_read32(task) == 0x00f92c98 &&
                    vm_read32(task + 0xc) == s_lifetime_probe_manager) {
                    taiko_plus::runtime().transition(s_launch_generation,
                        taiko_plus::State::Gameplay, taiko_plus::State::Results,
                        taiko_plus::EventKind::ResultsStarted);
                    std::fprintf(stderr, "[taiko_plus] native Results active task=%08X\n", task);
                    break;
                }
            }
        }
    }
    if (s_lifetime_probe_ticks == 0 && lifetime_trace_enabled())
        std::fprintf(stderr,
            "[taiko_plus_lifetime] frame runtime=%08X session=%08X "
            "entry_manager=%08X active_tasks=%u pending_tasks=%u tid=%u\n",
            s_sequence_runtime, s_sequence_runtime + 0xd8u,
            s_lifetime_probe_manager, vm_read32(s_sequence_runtime + 0x6cu),
            vm_read32(s_sequence_runtime + 0x78u), ctx->thread_id);
    /* This routine also completes deferred native scene destruction. Never
     * freeze it: the scene-owner facade exists only inside its stack frame. */
}

void taiko_pc_mode_frame_dispatch(ppu_context* ctx)
{
    if (!ctx || !taiko_pc_mode_is_active() || !taiko_pc_mode_is_standalone())
        return;
    /* Called after native traversal, before the facade is destroyed and
     * queued scene initialization/retirement is committed. */
    const uint32_t owner = static_cast<uint32_t>(ctx->gpr[1]) + 0x15cu;
    if (vm_read32(owner) != 0x00f9ae70u) return;
    // Runtime commands are serviced by the retained setup task, whose facade
    // has the correct native parent for queuing gameplay siblings.
}

void taiko_pc_mode_tick(ppu_context* ctx)
{
    if (!ctx || !taiko_pc_mode_is_active()) return;
    ++s_lifetime_probe_ticks;
    if (s_lifetime_probe_manager &&
        (s_lifetime_probe_ticks == 1u ||
         s_lifetime_probe_ticks % 600u == 0u))
        trace_manager("session-tick");
    taiko_plus::HostGuestCommand command;
    if (taiko_plus::runtime().try_pop_command(command)) {
        if (command.kind == taiko_plus::CommandKind::Abort) {
            taiko_plus::runtime().return_to_browser(
                command.match.generation);
        } else {
            prepare_match(command.match);
        }
    }
    advance_launch(static_cast<uint32_t>(ctx->gpr[4]));
    taiko_plus::HostGuestEvent event;
    while (taiko_plus::runtime().try_pop_event(event)) {
        if (event.kind == taiko_plus::EventKind::Failed)
            taiko_frontend_standalone_failure(event.error.detail.c_str());
    }
}

void taiko_pc_mode_init_hooks(void)
{
    ppu_register_function(kSequencePushTaskAddr, taiko_pc_mode_push_task_hook);
    std::fprintf(stderr,
                 "[taiko_pc_mode] Registered SequenceController hooks: push_task=0x%08X, update=0x%08X\n",
                 kSequencePushTaskAddr, kSequenceUpdateAddr);
}

namespace {
struct PcModeHookRegistrar {
    PcModeHookRegistrar() {
        ppu_set_project_register_hooks(taiko_pc_mode_init_hooks);
    }
} s_pc_mode_hook_registrar;
} // namespace

int taiko_pc_mode_results_return(uint32_t results, uint32_t owner)
{
    if (!taiko_pc_mode_is_active() || !taiko_pc_mode_is_standalone() ||
        !results || vm_read32(results) != 0x00f92c98 ||
        !owner || vm_read32(owner) != 0x00f9ae70 ||
        vm_read32(results + 0xc) != s_lifetime_probe_manager) return 0;
    const auto state = taiko_plus::runtime().state();
    if (state != taiko_plus::State::Gameplay && state != taiko_plus::State::Results)
        return 1; // A duplicate destination callback must not retire twice.
    if (static_cast<uint8_t>(native(0x008d427c, owner, results)))
        native(0x008ddd30, owner, results);
    taiko_plus::runtime().return_to_browser(taiko_plus::runtime().generation());
    taiko_host_audio_reacquire_menu();
    taiko_frontend_enter_song_select_shell();
    std::fprintf(stderr, "[taiko_plus] Results retired without Song Select allocation "
                 "results=%08X owner=%08X\n", results, owner);
    return 1;
}

int taiko_pc_mode_setup_tick(ppu_context* ctx)
{
    if (!ctx || !taiko_pc_mode_is_active() || !taiko_pc_mode_is_standalone() ||
        static_cast<uint32_t>(ctx->gpr[3]) != s_setup_anchor) return 0;
    // Keep this already initialized native task as the session's browser
    // anchor. Its original update would rerun setup and enter Song Select.
    // No Lumen or audio object is created by this idle task.
    taiko_pc_mode_tick(ctx);
    return 1;
}
