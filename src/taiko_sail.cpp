/* Taiko-local cellSail lifecycle shim.
 *
 * The generic ps3recomp cellSail stub models players as small host handles.
 * The real ABI instead passes a guest CellSailPlayer pointer and completes
 * lifecycle calls asynchronously through CellSailPlayerFuncNotified.  Taiko's
 * movie wrapper waits for those notifications; returning CELL_OK from Boot
 * without calling it leaves the game on its LOADING screen forever.
 *
 * We do not decode movies yet.  Initialize2 and Boot use the real guest-pointer
 * ABI, while stream calls normally retain the generic HLE's skip-movie
 * behavior.  TAIKO_SAIL_LIFECYCLE=1 enables the experimental complete stream
 * lifecycle for diagnosis; its synthetic SOURCE_EOS currently leaves Taiko's
 * wrapper in state 13 because no decoded-stream completion path exists.
 */

#include "ppu_recomp.h"

#include <array>
#include <chrono>
#include <cstdlib>
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
constexpr uint32_t kCellSailPlayerSetSoundAdapter = 0x1139A206u;
constexpr uint32_t kCellSailPlayerFinalize = 0x18B4629Du;
constexpr uint32_t kCellSailPlayerSetGraphicsAdapter = 0x18BCD21Bu;
constexpr uint32_t kCellSailGraphicsAdapterGetFrame = 0x0247C69Eu;
constexpr uint32_t kCellSailPlayerOpenStream = 0x34ECC1B9u;
constexpr uint32_t kCellSailPlayerSetParameter = 0x5F7C7A6Fu;
constexpr uint32_t kCellSailPlayerGetDescriptorCount = 0x752F8585u;
constexpr uint32_t kCellSailPlayerAddDescriptor = 0x7C8DFF3Bu;
constexpr uint32_t kCellSailSoundAdapterGetFrame = 0x7EB8D6B5u;
constexpr uint32_t kCellSailPlayerCloseStream = 0x85BEFFCCu;
constexpr uint32_t kCellSailPlayerRemoveDescriptor = 0x9897FBD1u;
constexpr uint32_t kCellSailPlayerIsPaused = 0xAAFA17B8u;
constexpr uint32_t kCellSailPlayerSetPaused = 0xD1D55A90u;
constexpr uint32_t kCellSailPlayerCreateDescriptor = 0xD7938B8Du;
constexpr uint32_t kCellSailPlayerStart = 0xE535B0D3u;
constexpr uint32_t kCellSailPlayerStop = 0xEBA8D4ECu;
constexpr uint32_t kCellSailPlayerSetRepeatMode = 0xFC5BAF8Au;
constexpr uint32_t kCellSailPlayerDestroyDescriptor = 0xFC839BD4u;
constexpr uint32_t kCellSysutilGetBgmPlaybackStatus = 0xA11552F6u;

constexpr uint32_t kCellSailErrorInvalidArg = 0x80610701u;
constexpr uint32_t kCellSailErrorInvalidState = 0x80610702u;
constexpr uint32_t kCellSailErrorEmpty = 0x80610705u;

constexpr uint32_t kEventPlayerCallCompleted = 2;
constexpr uint32_t kEventPlayerStateChanged = 3;
constexpr uint32_t kEventSourceEos = 8;
constexpr uint32_t kPlayerCallBoot = 1;
constexpr uint32_t kPlayerCallOpenStream = 2;
constexpr uint32_t kPlayerCallCloseStream = 3;
constexpr uint32_t kPlayerCallStart = 10;
constexpr uint32_t kPlayerCallStop = 11;
constexpr uint32_t kPlayerStateBootTransition = 1;
constexpr uint32_t kPlayerStateClosed = 2;
constexpr uint32_t kPlayerStateOpenTransition = 3;
constexpr uint32_t kPlayerStateOpened = 4;
constexpr uint32_t kPlayerStateStartTransition = 5;
constexpr uint32_t kPlayerStateRunning = 6;
constexpr uint32_t kPlayerStateStopTransition = 7;
constexpr uint32_t kPlayerStateCloseTransition = 8;

/* ffprobe's MPEG demuxer reports 29.5295 s for attract_cm_154.pam.  Taiko has
 * no other exercised movie path yet, so use that duration for the frame-less
 * lifecycle rather than completing Start synchronously and collapsing the
 * attract state machine into a tight loop. */
constexpr auto kMovieDuration = std::chrono::milliseconds(29530);

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
    uint32_t descriptor{};
    uint32_t sound_adapter{};
    uint32_t graphics_adapter{};
    int32_t repeat_mode{};
    bool descriptor_added{};
    bool paused{true};
    bool booted{};
    bool running{};
    std::chrono::steady_clock::time_point eos_deadline{};
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
        const uint32_t descriptor = player->descriptor;
        *player = {};
        player->descriptor = descriptor;
        player->callback = callback;
        player->callback_arg = callback_arg;
        player->self = self;
        return player;
    }

    for (auto& player : g_players) {
        if (!player.self) {
            player.self = self;
            player.callback = callback;
            player.callback_arg = callback_arg;
            player.descriptor = 0x7F000000u |
                                static_cast<uint32_t>(&player - g_players.data() + 1);
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

Player* find_player_by_adapter(uint32_t adapter)
{
    for (auto& player : g_players) {
        if (player.self &&
            (player.sound_adapter == adapter || player.graphics_adapter == adapter))
            return &player;
    }
    return nullptr;
}

void finish_due_movie(Player& player)
{
    if (!player.running || std::chrono::steady_clock::now() < player.eos_deadline)
        return;

    player.running = false;
    std::fprintf(stderr, "[taiko_sail] synthetic movie SOURCE_EOS player=0x%08X\n",
                 player.self);
    notify(player, kEventSourceEos, 0, 0, 0);
}

Player* require_player(ppu_context* ctx, const char* operation)
{
    const uint32_t self = static_cast<uint32_t>(ctx->gpr[3]);
    Player* player = find_player(self);
    if (!player) {
        std::fprintf(stderr, "[taiko_sail] %s self=0x%08X (uninitialized)\n",
                     operation, self);
        ctx->gpr[3] = kCellSailErrorInvalidState;
    }
    return player;
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

void set_sound_adapter(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerSetSoundAdapter");
    if (!player)
        return;
    player->sound_adapter = static_cast<uint32_t>(ctx->gpr[5]);
    ctx->gpr[3] = 0;
}

void set_graphics_adapter(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerSetGraphicsAdapter");
    if (!player)
        return;
    player->graphics_adapter = static_cast<uint32_t>(ctx->gpr[5]);
    ctx->gpr[3] = 0;
}

void set_parameter(ppu_context* ctx)
{
    if (!require_player(ctx, "PlayerSetParameter"))
        return;
    ctx->gpr[3] = 0;
}

void set_repeat_mode(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerSetRepeatMode");
    if (!player)
        return;
    player->repeat_mode = static_cast<int32_t>(ctx->gpr[4]);
    std::fprintf(stderr, "[taiko_sail] PlayerSetRepeatMode self=0x%08X mode=%d\n",
                 player->self, player->repeat_mode);
    /* This API returns the selected mode, not CELL_OK. */
    ctx->gpr[3] = static_cast<uint32_t>(player->repeat_mode);
}

void create_descriptor(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerCreateDescriptor");
    if (!player)
        return;
    const uint32_t stream_type = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t uri = static_cast<uint32_t>(ctx->gpr[6]);
    const uint32_t out = static_cast<uint32_t>(ctx->gpr[7]);
    if (!out) {
        ctx->gpr[3] = kCellSailErrorInvalidArg;
        return;
    }

    char guest_uri[320]{};
    if (uri) {
        for (uint32_t i = 0; i + 1 < sizeof(guest_uri); ++i) {
            guest_uri[i] = static_cast<char>(vm_read8(uri + i));
            if (!guest_uri[i])
                break;
        }
    }
    vm_write32(out, player->descriptor);
    player->descriptor_added = false;
    std::fprintf(stderr,
                 "[taiko_sail] PlayerCreateDescriptor self=0x%08X type=%u desc=0x%08X uri=\"%s\"\n",
                 player->self, stream_type, player->descriptor, guest_uri);
    ctx->gpr[3] = 0;
}

void add_descriptor(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerAddDescriptor");
    if (!player)
        return;
    const uint32_t descriptor = static_cast<uint32_t>(ctx->gpr[4]);
    if (descriptor != player->descriptor) {
        ctx->gpr[3] = kCellSailErrorInvalidArg;
        return;
    }
    player->descriptor_added = true;
    ctx->gpr[3] = 0;
}

void get_descriptor_count(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerGetDescriptorCount");
    if (!player)
        return;
    ctx->gpr[3] = player->descriptor_added ? 1 : 0;
}

void remove_descriptor(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerRemoveDescriptor");
    if (!player)
        return;
    const uint32_t out = static_cast<uint32_t>(ctx->gpr[4]);
    if (out)
        vm_write32(out, player->descriptor_added ? player->descriptor : 0);
    player->descriptor_added = false;
    ctx->gpr[3] = 0;
}

void destroy_descriptor(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerDestroyDescriptor");
    if (!player)
        return;
    if (static_cast<uint32_t>(ctx->gpr[4]) != player->descriptor) {
        ctx->gpr[3] = kCellSailErrorInvalidArg;
        return;
    }
    player->descriptor_added = false;
    ctx->gpr[3] = 0;
}

void open_stream(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerOpenStream");
    if (!player)
        return;
    if (!player->booted || !player->descriptor_added) {
        ctx->gpr[3] = kCellSailErrorInvalidState;
        return;
    }

    std::fprintf(stderr, "[taiko_sail] PlayerOpenStream self=0x%08X\n",
                 player->self);
    notify(*player, kEventPlayerStateChanged, 0,
           kPlayerStateOpenTransition, 0);
    notify(*player, kEventPlayerCallCompleted, kPlayerCallOpenStream, 0, 0);
    notify(*player, kEventPlayerStateChanged, 0, kPlayerStateOpened, 0);
    ctx->gpr[3] = 0;
}

void start(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerStart");
    if (!player)
        return;
    if (!player->descriptor_added) {
        ctx->gpr[3] = kCellSailErrorInvalidState;
        return;
    }

    std::fprintf(stderr, "[taiko_sail] PlayerStart self=0x%08X\n", player->self);
    notify(*player, kEventPlayerStateChanged, 0,
           kPlayerStateStartTransition, 0);
    notify(*player, kEventPlayerCallCompleted, kPlayerCallStart, 0, 0);
    player->running = true;
    player->paused = false;
    player->eos_deadline = std::chrono::steady_clock::now() + kMovieDuration;
    notify(*player, kEventPlayerStateChanged, 0, kPlayerStateRunning, 0);
    ctx->gpr[3] = 0;
}

void stop(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerStop");
    if (!player)
        return;
    std::fprintf(stderr, "[taiko_sail] PlayerStop self=0x%08X\n", player->self);
    player->running = false;
    notify(*player, kEventPlayerStateChanged, 0,
           kPlayerStateStopTransition, 0);
    notify(*player, kEventPlayerCallCompleted, kPlayerCallStop, 0, 0);
    notify(*player, kEventPlayerStateChanged, 0, kPlayerStateOpened, 0);
    ctx->gpr[3] = 0;
}

void close_stream(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerCloseStream");
    if (!player)
        return;
    std::fprintf(stderr, "[taiko_sail] PlayerCloseStream self=0x%08X\n",
                 player->self);
    player->running = false;
    notify(*player, kEventPlayerStateChanged, 0,
           kPlayerStateCloseTransition, 0);
    notify(*player, kEventPlayerCallCompleted, kPlayerCallCloseStream, 0, 0);
    notify(*player, kEventPlayerStateChanged, 0, kPlayerStateClosed, 0);
    ctx->gpr[3] = 0;
}

void is_paused(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerIsPaused");
    if (!player)
        return;
    ctx->gpr[3] = player->paused ? 1 : 0;
}

void set_paused(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerSetPaused");
    if (!player)
        return;
    player->paused = static_cast<uint32_t>(ctx->gpr[4]) != 0;
    ctx->gpr[3] = 0;
}

void adapter_get_frame(ppu_context* ctx)
{
    const uint32_t adapter = static_cast<uint32_t>(ctx->gpr[3]);
    if (Player* player = find_player_by_adapter(adapter))
        finish_due_movie(*player);
    /* There is deliberately no decoded video/audio frame to return. */
    ctx->gpr[3] = kCellSailErrorEmpty;
}

void finalize(ppu_context* ctx)
{
    Player* player = require_player(ctx, "PlayerFinalize");
    if (!player)
        return;
    const uint32_t descriptor = player->descriptor;
    *player = {};
    player->descriptor = descriptor;
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

    /* Until the movie decoder exists, the generic stream HLE's failed-open
     * path is the known-good contract for Taiko: it skips the black attract
     * movie and lets the scene's audio/timeline drive the next cycle.  The
     * pointer-correct lifecycle remains available as an explicit A/B switch. */
    const char* lifecycle = std::getenv("TAIKO_SAIL_LIFECYCLE");
    if (!lifecycle || !*lifecycle || *lifecycle == '0') {
        std::fprintf(stderr,
                     "[taiko_sail] stream lifecycle disabled; movies use skip path\n");
        ps3_hle_register_ctx(kCellSysutilGetBgmPlaybackStatus,
                             "cellSysutilGetBgmPlaybackStatus",
                             get_bgm_playback_status);
        return;
    }

    std::fprintf(stderr, "[taiko_sail] experimental stream lifecycle enabled\n");
    ps3_hle_register_ctx(kCellSailPlayerSetSoundAdapter,
                         "cellSailPlayerSetSoundAdapter", set_sound_adapter);
    ps3_hle_register_ctx(kCellSailPlayerFinalize,
                         "cellSailPlayerFinalize", finalize);
    ps3_hle_register_ctx(kCellSailPlayerSetGraphicsAdapter,
                         "cellSailPlayerSetGraphicsAdapter", set_graphics_adapter);
    ps3_hle_register_ctx(kCellSailGraphicsAdapterGetFrame,
                         "cellSailGraphicsAdapterGetFrame", adapter_get_frame);
    ps3_hle_register_ctx(kCellSailPlayerOpenStream,
                         "cellSailPlayerOpenStream", open_stream);
    ps3_hle_register_ctx(kCellSailPlayerSetParameter,
                         "cellSailPlayerSetParameter", set_parameter);
    ps3_hle_register_ctx(kCellSailPlayerGetDescriptorCount,
                         "cellSailPlayerGetDescriptorCount", get_descriptor_count);
    ps3_hle_register_ctx(kCellSailPlayerAddDescriptor,
                         "cellSailPlayerAddDescriptor", add_descriptor);
    ps3_hle_register_ctx(kCellSailSoundAdapterGetFrame,
                         "cellSailSoundAdapterGetFrame", adapter_get_frame);
    ps3_hle_register_ctx(kCellSailPlayerCloseStream,
                         "cellSailPlayerCloseStream", close_stream);
    ps3_hle_register_ctx(kCellSailPlayerRemoveDescriptor,
                         "cellSailPlayerRemoveDescriptor", remove_descriptor);
    ps3_hle_register_ctx(kCellSailPlayerIsPaused,
                         "cellSailPlayerIsPaused", is_paused);
    ps3_hle_register_ctx(kCellSailPlayerSetPaused,
                         "cellSailPlayerSetPaused", set_paused);
    ps3_hle_register_ctx(kCellSailPlayerCreateDescriptor,
                         "cellSailPlayerCreateDescriptor", create_descriptor);
    ps3_hle_register_ctx(kCellSailPlayerStart,
                         "cellSailPlayerStart", start);
    ps3_hle_register_ctx(kCellSailPlayerStop,
                         "cellSailPlayerStop", stop);
    ps3_hle_register_ctx(kCellSailPlayerSetRepeatMode,
                         "cellSailPlayerSetRepeatMode", set_repeat_mode);
    ps3_hle_register_ctx(kCellSailPlayerDestroyDescriptor,
                         "cellSailPlayerDestroyDescriptor", destroy_descriptor);
    ps3_hle_register_ctx(kCellSysutilGetBgmPlaybackStatus,
                         "cellSysutilGetBgmPlaybackStatus",
                         get_bgm_playback_status);
}

} // namespace
