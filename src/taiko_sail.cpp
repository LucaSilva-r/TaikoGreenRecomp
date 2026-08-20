/* Taiko-local cellSail lifecycle shim.
 *
 * The generic ps3recomp cellSail stub models players as small host handles.
 * The real ABI instead passes a guest CellSailPlayer pointer and completes
 * lifecycle calls asynchronously through CellSailPlayerFuncNotified.  Taiko's
 * movie wrapper waits for those notifications; returning CELL_OK from Boot
 * without calling it leaves the game on its LOADING screen forever.
 *
 * We do not decode movies yet.  This shim only implements the real guest ABI
 * for Initialize2 and Boot, and reports Boot complete immediately.  Later
 * stream calls can use the same mechanism to skip playback cleanly.
 */

#include "ppu_recomp.h"

#include <array>
#include <cstdint>
#include <cstdio>

extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name,
                                      void (*handler)(ppu_context*));
extern "C" uint64_t ppu_guest_call(uint32_t opd, uint64_t a0, uint64_t a1,
                                   uint64_t a2, uint64_t a3);

namespace {

constexpr uint32_t kCellSailPlayerInitialize2 = 0x23654375u;
constexpr uint32_t kCellSailMemAllocatorInitialize = 0x346EBBA3u;
constexpr uint32_t kCellSailPlayerBoot = 0xBDF21B0Fu;
constexpr uint32_t kCellSysutilGetBgmPlaybackStatus = 0xA11552F6u;

constexpr uint32_t kEventPlayerCallCompleted = 2;
constexpr uint32_t kEventPlayerStateChanged = 3;
constexpr uint32_t kPlayerCallBoot = 1;
constexpr uint32_t kPlayerStateBootTransition = 1;
constexpr uint32_t kPlayerStateClosed = 2;

/* Guest CellSailPlayer layout from the PS3 SDK ABI.  All pointer and integer
 * members are 32-bit big-endian; the two state flags are single bytes. */
constexpr uint32_t kPlayerAllocatorOffset = 0;
constexpr uint32_t kPlayerCallbackOffset = 8;
constexpr uint32_t kPlayerCallbackArgOffset = 12;
constexpr uint32_t kPlayerAttributeOffset = 16;
constexpr uint32_t kPlayerResourceOffset = 48;
constexpr uint32_t kPlayerPausedOffset = 84;
constexpr uint32_t kPlayerBootedOffset = 85;

struct Player {
    uint32_t self{};
    uint32_t callback{};
    uint32_t callback_arg{};
    bool booted{};
};

std::array<Player, 4> g_players{};

Player* find_player(uint32_t self)
{
    for (auto& player : g_players)
        if (player.self == self)
            return &player;
    return nullptr;
}

Player* remember_player(uint32_t self, uint32_t callback, uint32_t callback_arg)
{
    if (Player* player = find_player(self)) {
        player->callback = callback;
        player->callback_arg = callback_arg;
        player->booted = false;
        return player;
    }

    for (auto& player : g_players) {
        if (!player.self) {
            player.self = self;
            player.callback = callback;
            player.callback_arg = callback_arg;
            return &player;
        }
    }
    return nullptr;
}

uint64_t event(uint32_t major, uint32_t minor)
{
    return (static_cast<uint64_t>(major) << 32) | minor;
}

void copy_guest_words(uint32_t destination, uint32_t source, uint32_t size)
{
    for (uint32_t offset = 0; offset < size; offset += 4)
        vm_write32(destination + offset, vm_read32(source + offset));
}

void notify(const Player& player, uint32_t major, uint32_t minor,
            uint64_t arg0, uint64_t arg1)
{
    if (!player.callback) {
        std::fprintf(stderr,
                     "[taiko_sail] missing callback for player=0x%08X event=%u:%u\n",
                     player.self, major, minor);
        return;
    }

    std::fprintf(stderr,
                 "[taiko_sail] notify callback=0x%08X arg=0x%08X event=%u:%u arg0=%llu\n",
                 player.callback, player.callback_arg, major, minor,
                 static_cast<unsigned long long>(arg0));
    ppu_guest_call(player.callback, player.callback_arg,
                   event(major, minor), arg0, arg1);
}

void initialize2(ppu_context* ctx)
{
    const uint32_t self = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t allocator = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t callback = static_cast<uint32_t>(ctx->gpr[5]);
    const uint32_t callback_arg = static_cast<uint32_t>(ctx->gpr[6]);
    const uint32_t attribute = static_cast<uint32_t>(ctx->gpr[7]);
    const uint32_t resource = static_cast<uint32_t>(ctx->gpr[8]);

    Player* player = remember_player(self, callback, callback_arg);
    std::fprintf(stderr,
                 "[taiko_sail] PlayerInitialize2 self=0x%08X callback=0x%08X arg=0x%08X\n",
                 self, callback, callback_arg);
    if (!self || !allocator || !attribute || !resource || !player) {
        ctx->gpr[3] = 0x806107F0u; /* CELL_SAIL_ERROR_MEMORY */
        return;
    }

    copy_guest_words(self + kPlayerAllocatorOffset, allocator, 8);
    vm_write32(self + kPlayerCallbackOffset, callback);
    vm_write32(self + kPlayerCallbackArgOffset, callback_arg);
    copy_guest_words(self + kPlayerAttributeOffset, attribute, 32);
    copy_guest_words(self + kPlayerResourceOffset, resource, 16);
    vm_write8(self + kPlayerPausedOffset, 1);
    vm_write8(self + kPlayerBootedOffset, 0);

    /* The real library reports INITIALIZED here.  In this recomp the callback
     * runs on our nested guest-call trampoline while Taiko's catalog loader is
     * still constructing its observer graph; delivering it synchronously
     * starts that graph early and reproducibly corrupts the later XML parse.
     * Taiko already proceeds to Boot from the successful return value, so defer
     * notifications until Boot, which is the transition it actually waits on. */
    ctx->gpr[3] = 0;
}

void mem_allocator_initialize(ppu_context* ctx)
{
    const uint32_t self = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t callbacks = static_cast<uint32_t>(ctx->gpr[4]);
    std::fprintf(stderr,
                 "[taiko_sail] MemAllocatorInitialize self=0x%08X callbacks=0x%08X\n",
                 self, callbacks);
    if (!self) {
        ctx->gpr[3] = 0x80610701u; /* CELL_SAIL_ERROR_INVALID_ARG */
        return;
    }

    /* CellSailMemAllocator is two guest big-endian u32 fields.  The API only
     * initializes `callbacks`; pArg is owned by the caller.  The generic HLE
     * used native 64-bit function pointers here and overwrote 16 bytes beyond
     * this structure, including Taiko's PlayerInitialize2 attributes. */
    vm_write32(self, callbacks);
    ctx->gpr[3] = 0;
}

void boot(ppu_context* ctx)
{
    const uint32_t self = static_cast<uint32_t>(ctx->gpr[3]);
    Player* player = find_player(self);
    std::fprintf(stderr, "[taiko_sail] PlayerBoot self=0x%08X userParam=%llu%s\n",
                 self, static_cast<unsigned long long>(ctx->gpr[4]),
                 player ? "" : " (uninitialized)");
    if (!player) {
        ctx->gpr[3] = 0x80610702u; /* CELL_SAIL_ERROR_INVALID_STATE */
        return;
    }

    notify(*player, kEventPlayerStateChanged, 0,
           kPlayerStateBootTransition, 0);
    player->booted = true;
    vm_write8(self + kPlayerBootedOffset, 1);
    notify(*player, kEventPlayerCallCompleted, kPlayerCallBoot, 0, 0);

    /* Boot leaves a Sail player ready but with no stream open.  Taiko's Sail
     * wrapper waits for this stable state before changing its own state from
     * BOOTING to READY.  The abbreviated RPCS3 implementation omits it, which
     * is harmless there for many titles but makes Taiko submit Boot every
     * frame. */
    notify(*player, kEventPlayerStateChanged, 0, kPlayerStateClosed, 0);
    ctx->gpr[3] = 0;
}

void get_bgm_playback_status(ppu_context* ctx)
{
    const uint32_t status = static_cast<uint32_t>(ctx->gpr[3]);
    if (!status) {
        ctx->gpr[3] = 0x8002B102u; /* CELL_SYSUTIL_ERROR_VALUE */
        return;
    }

    /* The generic ABI bridge cannot pass a guest effective address as a native
     * pointer.  cellSysutil's old implementation dereferenced the raw value
     * (Taiko uses a worker-thread stack around 0xD0030000), causing a host AV.
     * Store the big-endian result through the guest VM accessor instead. */
    vm_write32(status, 0); /* CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP */
    ctx->gpr[3] = 0;
}

__attribute__((constructor)) void register_taiko_sail()
{
    std::fprintf(stderr, "[taiko_sail] registering cellSail overrides\n");
    ps3_hle_register_ctx(kCellSailPlayerInitialize2,
                         "cellSailPlayerInitialize2", initialize2);
    ps3_hle_register_ctx(kCellSailMemAllocatorInitialize,
                         "cellSailMemAllocatorInitialize",
                         mem_allocator_initialize);
    ps3_hle_register_ctx(kCellSailPlayerBoot,
                         "cellSailPlayerBoot", boot);
    ps3_hle_register_ctx(kCellSysutilGetBgmPlaybackStatus,
                         "cellSysutilGetBgmPlaybackStatus",
                         get_bgm_playback_status);
}

} // namespace
