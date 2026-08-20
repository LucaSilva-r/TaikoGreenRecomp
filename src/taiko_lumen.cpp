/* Title-local Lumen state fixes.
 *
 * Green's player-entry movie creates its title as sprite 817 (26 frames), but
 * in the offline recomp path the ActionScript scene transition never releases
 * that child from its authored frame-0 Stop action.  The movie and texture are
 * otherwise intact: releasing the exact live clip lets it animate normally to
 * frame 20, matching the reference runtime.
 */

#include "ppu_recomp.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" void ppu_set_project_register_hooks(void (*register_hooks)(void));

namespace {

std::atomic<int> g_shape_trace_left{0};
std::atomic<uint32_t> g_shape_trace_seq{0};

constexpr uint32_t kSpriteAdvance = 0x003DF910u;
constexpr uint32_t kSpriteVtable = 0x00F09030u;
constexpr uint32_t kPlayerEntryTitleId = 817u;
constexpr uint32_t kPlayerEntryTitleFrames = 26u;
constexpr uint32_t kOnlineCheckPtr = 0x01028F1Cu;
constexpr uint32_t kNetworkFlagsPtr = 0x010290A4u;

void release_offline_player_entry_title(ppu_context* ctx)
{
    const uint32_t object = static_cast<uint32_t>(ctx->gpr[3]);
    static bool released = false;
    static const char* setting = std::getenv("TAIKO_OFFLINE_COMPLETE");
    static const bool offline_complete = setting && std::strcmp(setting, "0") != 0;

    if (!released && offline_complete && object &&
        vm_read32(object) == kSpriteVtable &&
        vm_read32(object + 0xBC) == kPlayerEntryTitleId &&
        vm_read32(object + 0x140) == 0 &&
        vm_read32(object + 0x144) == kPlayerEntryTitleFrames &&
        vm_read8(object + 0x14C) != 0) {
        const uint32_t online = vm_read32(kOnlineCheckPtr);
        const uint32_t network_flags = vm_read32(kNetworkFlagsPtr);
        if (online && network_flags && vm_read32(online) == 3 &&
            (vm_read64(network_flags) & 0x10000000ull) != 0) {
            vm_write8(object + 0xC8, 1);
            vm_write8(object + 0x14C, 0);
            released = true;
            std::fprintf(stderr,
                         "[taiko_lumen] released offline player-entry title "
                         "sprite object=%08X\n", object);
        }
    }

    func_003DF910(ctx);
}

void register_taiko_lumen_hooks()
{
    ppu_register_function(kSpriteAdvance, release_offline_player_entry_title);
    std::fprintf(stderr,
                 "[taiko_lumen] installed player-entry title Sprite::AdvanceFrame hook\n");
}

__attribute__((constructor))
void configure_taiko_lumen_hooks()
{
    ppu_set_project_register_hooks(register_taiko_lumen_hooks);
}

} // namespace

extern "C" void taiko_lumen_trace_arm()
{
    const char* setting = std::getenv("TAIKO_LUMEN_TRACE_SHAPES");
    if (!setting || std::strcmp(setting, "0") == 0)
        return;

    int count = std::atoi(setting);
    if (count <= 0)
        count = 1000;
    g_shape_trace_seq.store(0, std::memory_order_relaxed);
    g_shape_trace_left.store(count, std::memory_order_release);
    std::fprintf(stderr, "[LUMENSHAPE] armed count=%d\n", count);
}

extern "C" void taiko_lumen_trace_shape(ppu_context* ctx)
{
    int left = g_shape_trace_left.load(std::memory_order_acquire);
    while (left > 0 &&
           !g_shape_trace_left.compare_exchange_weak(
               left, left - 1, std::memory_order_acq_rel)) {}
    if (left <= 0)
        return;

    const uint32_t object = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t owner = object ? vm_read32(object + 0x14) : 0;
    const uint32_t primitive_list = object ? vm_read32(object + 0xD0) : 0;
    const uint32_t primitive_head = primitive_list ? vm_read32(primitive_list) : 0;
    const uint32_t primitive_base = primitive_list ? vm_read32(primitive_list + 4) : 0;
    uint32_t primitive_count = primitive_head ? vm_read32(primitive_head + 0x14) : 0;
    if (primitive_count > 1024)
        primitive_count = 0;

    uint32_t hash = 2166136261u;
    const uint32_t byte_count = primitive_count * 0x50u;
    for (uint32_t i = 0; i < byte_count; ++i) {
        hash ^= vm_read8(primitive_base + i);
        hash *= 16777619u;
    }

    char first_record[161];
    static constexpr char hex[] = "0123456789abcdef";
    const uint32_t first_bytes = byte_count < 80 ? byte_count : 80;
    for (uint32_t i = 0; i < first_bytes; ++i) {
        const uint8_t value = vm_read8(primitive_base + i);
        first_record[i * 2] = hex[value >> 4];
        first_record[i * 2 + 1] = hex[value & 15];
    }
    first_record[first_bytes * 2] = '\0';

    std::fprintf(stderr,
                 "[LUMENSHAPE] seq=%u object=%08X lr=%08X id=%u owner=%08X "
                 "owner_id=%u field_c0=%u flags=%08X resource=%08X context=%08X "
                 "prim_count=%u prim_hash=%08X prim_bytes=%s\n",
                 g_shape_trace_seq.fetch_add(1, std::memory_order_relaxed),
                 object, static_cast<uint32_t>(ctx->lr),
                 object ? vm_read32(object + 0xBC) : 0, owner,
                 owner ? vm_read32(owner + 0xBC) : 0,
                 object ? vm_read32(object + 0xC0) : 0,
                 object ? vm_read32(object + 0xC8) : 0,
                 object ? vm_read32(object + 8) : 0,
                 object ? vm_read32(object + 0xC) : 0,
                 primitive_count, hash, first_record);
}
