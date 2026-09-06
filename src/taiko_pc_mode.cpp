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
constexpr uint32_t kSequenceUpdateAddr = 0x008DA730u;
constexpr uint32_t kEntryStateOffset = 0x14u;
constexpr uint32_t kEntryStateEnd = 39u;
constexpr uint32_t kEntryStateTerm = 40u;

std::atomic<bool> s_pc_mode_active{false};
std::atomic<bool> s_pending_pc_mode{false};
std::atomic<bool> s_entry_handoff_armed{false};
uint32_t s_lifetime_probe_manager = 0;
uint64_t s_lifetime_probe_ticks = 0;

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
    std::fprintf(stderr, "[taiko_pc_mode] PC Mode deactivated\n");
}

void taiko_pc_mode_push_task_hook(ppu_context* ctx)
{
    if (!ctx) return;
    const uint32_t controller = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t task = static_cast<uint32_t>(ctx->gpr[4]);

    if (s_entry_handoff_armed.exchange(false, std::memory_order_acq_rel)) {
        s_pending_pc_mode.store(false, std::memory_order_release);
        const uint32_t task_vtable = task ? vm_read32(task) : 0;
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

void taiko_pc_mode_update_hook(ppu_context* ctx)
{
    if (!ctx) return;

    if (taiko_pc_mode_is_active() && taiko_pc_mode_is_standalone()) {
        taiko_pc_mode_tick(ctx);
        // Do not update arcade tasks while host PC mode has exclusive ownership
        ctx->gpr[3] = 1;
        return;
    }

    func_008DA730(ctx);
}

void taiko_pc_mode_tick(ppu_context* ctx)
{
    if (!ctx || !taiko_pc_mode_is_active()) return;
    if (s_lifetime_probe_manager &&
        (++s_lifetime_probe_ticks == 1u ||
         s_lifetime_probe_ticks % 600u == 0u))
        trace_manager("browser-idle");
    taiko_plus::HostGuestCommand command;
    if (taiko_plus::runtime().try_pop_command(command)) {
        if (command.kind == taiko_plus::CommandKind::Abort) {
            taiko_plus::runtime().return_to_browser(
                command.match.generation);
        } else {
            /* The queue is now the only cross-thread launch boundary. Native
             * manager/player construction will replace this structured,
             * browser-safe failure once its retain/release contract has been
             * live-proved. */
            taiko_plus::runtime().fail(
                command.match.generation,
                taiko_plus::GuestErrorCode::GuestBootstrapUnavailable,
                "native GameEnso bootstrap contract is not yet validated");
        }
    }
    taiko_plus::HostGuestEvent event;
    while (taiko_plus::runtime().try_pop_event(event)) {
        if (event.kind == taiko_plus::EventKind::Failed)
            taiko_frontend_standalone_failure(event.error.detail.c_str());
    }
}

void taiko_pc_mode_init_hooks(void)
{
    ppu_register_function(kSequencePushTaskAddr, taiko_pc_mode_push_task_hook);
    ppu_register_function(kSequenceUpdateAddr, taiko_pc_mode_update_hook);
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
