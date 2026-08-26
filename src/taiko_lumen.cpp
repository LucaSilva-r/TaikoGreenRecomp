/* Title-local Lumen state fixes.
 *
 * Green's player-entry movie creates its title as sprite 817 (26 frames), but
 * in the offline recomp path the ActionScript scene transition never releases
 * that child from its authored frame-0 Stop action.  The movie and texture are
 * otherwise intact: releasing the exact live clip lets it animate normally to
 * frame 20, matching the reference runtime.
 */

#include "ppu_recomp.h"

#include <ps3emu/host_platform.h>

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
std::atomic<uint32_t> g_animation_scale_bits{0x3F800000u};
std::atomic<uint64_t> g_animation_previous_flip_ns{0};
std::atomic<uint32_t> g_animation_don3d_calls{0};
std::atomic<uint32_t> g_animation_lumen_calls{0};
std::atomic<uint32_t> g_animation_face_skips{0};

constexpr uint32_t kSpriteAdvance = 0x003DF910u;
constexpr uint32_t kSpriteVtable = 0x00F09030u;
constexpr uint32_t kPlayerEntryTitleId = 817u;
constexpr uint32_t kPlayerEntryTitleFrames = 26u;
constexpr uint32_t kOnlineCheckPtr = 0x01028F1Cu;
constexpr uint32_t kNetworkFlagsPtr = 0x010290A4u;
constexpr uint32_t kFaceDonWrapperVtable = 0x00F87868u;
constexpr uint32_t kFaceKatsuWrapperVtable = 0x00F878A0u;
constexpr uint64_t kAnimationTimingGapNs = 250000000ull;
constexpr float kAnimationScaleMax = 4.0f;

extern "C" int ps3_frame_boot_fast_is_done(void);

bool animation_timing_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("TAIKO_ANIMATION_TIMING");
        return value && value[0] && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

uint32_t float_bits(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float animation_scale()
{
    const uint32_t bits =
        g_animation_scale_bits.load(std::memory_order_acquire);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool is_deferred_face_timeline(uint32_t player)
{
    if (!player)
        return false;
    const uint32_t owner = vm_read32(player + 8);
    if (owner < 0x20000000u || owner >= 0x50000000u)
        return false;
    const uint32_t vtable = vm_read32(owner);
    return vtable == kFaceDonWrapperVtable ||
           vtable == kFaceKatsuWrapperVtable;
}

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
                 "[taiko_lumen] installed player-entry Sprite hook; "
                 "elapsed animation timing %s\n",
                 animation_timing_enabled() ? "enabled" : "disabled");
}

__attribute__((constructor))
void configure_taiko_lumen_hooks()
{
    ppu_set_project_register_hooks(register_taiko_lumen_hooks);
}

} // namespace

extern "C" void taiko_don3d_scale_frame_delta(ppu_context* ctx)
{
    if (!ctx || !animation_timing_enabled())
        return;
    g_animation_don3d_calls.fetch_add(1, std::memory_order_relaxed);
    ctx->fpr[31] = static_cast<double>(static_cast<float>(
        ctx->fpr[31] * static_cast<double>(animation_scale())));
}

extern "C" void taiko_project_flip_command()
{
    const uint64_t now = ps3_host_monotonic_ns();
    if (!animation_timing_enabled() || !ps3_frame_boot_fast_is_done()) {
        g_animation_previous_flip_ns.store(now, std::memory_order_relaxed);
        g_animation_scale_bits.store(0x3F800000u,
                                     std::memory_order_release);
        return;
    }

    const uint64_t previous =
        g_animation_previous_flip_ns.exchange(now, std::memory_order_relaxed);
    float scale = 1.0f;
    if (previous && now > previous) {
        const uint64_t delta = now - previous;
        if (delta < kAnimationTimingGapNs) {
            scale = static_cast<float>(delta) * 0.00000006f;
            if (scale > kAnimationScaleMax)
                scale = kAnimationScaleMax;
        }
    }
    g_animation_scale_bits.store(float_bits(scale),
                                 std::memory_order_release);

    if (std::getenv("TAIKO_ANIMATION_TIMING_TRACE")) {
        static uint64_t report_ns;
        static float minimum = kAnimationScaleMax;
        static float maximum = 0.0f;
        static double total;
        static unsigned samples;
        if (scale < minimum) minimum = scale;
        if (scale > maximum) maximum = scale;
        total += scale;
        ++samples;
        if (!report_ns) report_ns = now;
        if (now - report_ns >= 1000000000ull) {
            const uint32_t don3d_calls =
                g_animation_don3d_calls.exchange(0,
                                                  std::memory_order_relaxed);
            const uint32_t lumen_calls =
                g_animation_lumen_calls.exchange(0,
                                                  std::memory_order_relaxed);
            const uint32_t face_skips =
                g_animation_face_skips.exchange(0,
                                                 std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[ANIMATION-TIMING] samples=%u scale=%.3f..%.3f "
                         "mean=%.3f calls(don3d=%u lumen=%u face-skip=%u)\n",
                         samples, minimum, maximum,
                         samples ? total / samples : 0.0,
                         don3d_calls, lumen_calls, face_skips);
            report_ns = now;
            minimum = kAnimationScaleMax;
            maximum = 0.0f;
            total = 0.0;
            samples = 0;
        }
    }
}

extern "C" void taiko_lumen_scale_frame_delta(ppu_context* ctx)
{
    if (!ctx || !animation_timing_enabled())
        return;
    if (is_deferred_face_timeline(static_cast<uint32_t>(ctx->gpr[30]))) {
        g_animation_face_skips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_animation_lumen_calls.fetch_add(1, std::memory_order_relaxed);
    ctx->fpr[1] = static_cast<double>(static_cast<float>(
        ctx->fpr[1] * static_cast<double>(animation_scale())));
}

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
