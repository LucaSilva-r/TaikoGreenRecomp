#include "taiko_pc_mode.h"
#include "taiko_frontend.h"
#include "ppu_recomp.h"
#include "taiko_overlay.h"

#include <atomic>
#include <cstdio>

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

} // namespace

int taiko_pc_mode_is_active(void)
{
    return s_pc_mode_active.load(std::memory_order_acquire) ? 1 : 0;
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
    std::fprintf(stderr,
                 "[taiko_pc_mode] host PC Mode activated; "
                 "SequenceController=%08X\n", controller);

    taiko_overlay_clear();
    taiko_frontend_enter_song_select_shell();
}

void taiko_pc_mode_deactivate(void)
{
    s_pc_mode_active.store(false, std::memory_order_release);
    s_pending_pc_mode.store(false, std::memory_order_release);
    s_entry_handoff_armed.store(false, std::memory_order_release);
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
        std::fprintf(stderr,
                     "[taiko_pc_mode] suppressing post-Entry arcade task "
                     "controller=%08X task=%08X vtable=%08X\n",
                     controller, task, task_vtable);

        taiko_pc_mode_activate(controller);

        ctx->gpr[3] = 1;
        return;
    }

    func_008DA500(ctx);
}

void taiko_pc_mode_update_hook(ppu_context* ctx)
{
    if (!ctx) return;

    if (taiko_pc_mode_is_active()) {
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
