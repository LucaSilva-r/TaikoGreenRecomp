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
std::atomic<uint32_t> g_animation_tick_quanta{1};
std::atomic<uint64_t> g_animation_previous_flip_ns{0};
std::atomic<uint64_t> g_animation_flip_sequence{0};
std::atomic<uint32_t> g_animation_don3d_calls{0};
std::atomic<uint32_t> g_animation_lumen_calls{0};
float g_animation_tick_accumulator = 0.0f;
uint64_t g_face_seek_flip[5] = {};
uint64_t g_face_seek_last_ns[5] = {};
bool g_face_seek_allow[5] = {};

constexpr uint32_t kSpriteAdvance = 0x003DF910u;
constexpr uint32_t kSpriteVtable = 0x00F09030u;
constexpr uint32_t kPlayerEntryTitleId = 817u;
constexpr uint32_t kPlayerEntryTitleFrames = 26u;
constexpr uint32_t kOnlineCheckPtr = 0x01028F1Cu;
constexpr uint32_t kNetworkFlagsPtr = 0x010290A4u;
constexpr uint32_t kNormalNoteFaceCharacter = 7u;
constexpr uint32_t kNormalNoteFaceFrames = 90u;
constexpr uint32_t kBigNoteFaceCharacter = 5u;
constexpr uint32_t kBigNoteFaceFrames = 95u;
constexpr uint64_t kAnimationTimingGapNs = 250000000ull;
constexpr uint64_t kFaceSyncMinimumNs = 280000000ull;
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

bool guest_string_equals(uint32_t address, const char* expected)
{
    if (!address || !expected)
        return false;
    for (uint32_t i = 0; i < 16; ++i) {
        const char actual = static_cast<char>(vm_read8(address + i));
        if (actual != expected[i])
            return false;
        if (actual == '\0')
            return true;
    }
    return false;
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

extern "C" int taiko_lumen_skip_redundant_face_seek(ppu_context* ctx)
{
    if (!ctx || !animation_timing_enabled())
        return 0;

    const uint32_t object = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t label = static_cast<uint32_t>(ctx->gpr[4]);
    if (!object || vm_read32(object) != kSpriteVtable)
        return 0;

    const uint32_t sprite_id = vm_read32(object + 0xBC);
    const uint32_t frame_count = vm_read32(object + 0x144);
    const bool normal = sprite_id == kNormalNoteFaceCharacter &&
                        frame_count == kNormalNoteFaceFrames;
    const bool big = sprite_id == kBigNoteFaceCharacter &&
                     frame_count == kBigNoteFaceFrames;
    if (!normal && !big)
        return 0;

    uint32_t first;
    uint32_t last;
    uint32_t state;
    /* The live label lookup maps these names one tier later than the raw LMF
     * decompiler's label summary suggests.  The gameplay controller reissues
     * the current label on a frame-counted timer.  At high refresh that seek
     * lands before the face reaches its first visible keyframe and continually
     * restarts it. */
    if (guest_string_equals(label, "level01")) {
        first = 0;
        last = 9;
        state = 0;
    } else if (guest_string_equals(label, "level02")) {
        first = 10;
        last = 49;
        state = 1;
    } else if (guest_string_equals(label, "level03")) {
        first = 50;
        last = 69;
        state = 2;
    } else if (guest_string_equals(label, "level04")) {
        first = 70;
        last = 89;
        state = 3;
    } else if (big && guest_string_equals(label, "onp_wait")) {
        first = 90;
        last = 94;
        state = 4;
    } else {
        return 0;
    }

    /* Unlike the normal face movie, the big-note movie does not stop at the
     * end of level01: if its controller's repeated low-combo seek is filtered
     * out, the timeline falls through into the animated level02 range.  Keep
     * that authored seek intact.  Animated tiers still use the real-time
     * phase-locking below. */
    if (big && state == 0)
        return 0;

    const uint32_t frame = vm_read32(object + 0x140);
    const bool stopped = vm_read8(object + 0x14C) != 0;
    const bool redundant = frame >= first && frame <= last &&
                           (first == 0 || (!stopped && frame < last));
    const uint64_t flip =
        g_animation_flip_sequence.load(std::memory_order_acquire);
    const uint64_t now = ps3_host_monotonic_ns();
    if (!redundant) {
        /* State changes and authored per-face loops always execute, but a
         * newly spawned note must not move the shared synchronization phase. */
        if (g_face_seek_last_ns[state] == 0)
            g_face_seek_last_ns[state] = now;
        return 0;
    }

    /* The repeated seek is intentional: it periodically phase-locks all note
     * faces.  Use a stable real-time floor rather than accumulating the noisy
     * instantaneous frame scale.  Every face visited during one guest frame
     * shares the same decision. */
    if (flip != g_face_seek_flip[state]) {
        g_face_seek_flip[state] = flip;
        const uint64_t previous = g_face_seek_last_ns[state];
        g_face_seek_allow[state] =
            previous == 0 || now - previous >= kFaceSyncMinimumNs;
        if (g_face_seek_allow[state])
            g_face_seek_last_ns[state] = now;
    }
    return g_face_seek_allow[state] ? 0 : 1;
}

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
    g_animation_flip_sequence.fetch_add(1, std::memory_order_release);
    if (!animation_timing_enabled() || !ps3_frame_boot_fast_is_done()) {
        g_animation_previous_flip_ns.store(now, std::memory_order_relaxed);
        g_animation_scale_bits.store(0x3F800000u,
                                     std::memory_order_release);
        g_animation_tick_accumulator = 0.0f;
        g_animation_tick_quanta.store(1, std::memory_order_release);
        return;
    }

    const uint64_t previous =
        g_animation_previous_flip_ns.exchange(now, std::memory_order_relaxed);
    float scale = 1.0f;
    uint32_t tick_quanta = 1;
    if (previous && now > previous) {
        const uint64_t delta = now - previous;
        if (delta < kAnimationTimingGapNs) {
            scale = static_cast<float>(delta) * 0.00000006f;
            if (scale > kAnimationScaleMax)
                scale = kAnimationScaleMax;
        }

        /* Lumen's complete player update is transactional: preparation,
         * timeline advance, actions, and child traversal must all happen as
         * one authored tick. Convert elapsed time to whole 60 Hz quanta here
         * instead of fractionally throttling individual Sprite callbacks. */
        g_animation_tick_accumulator += scale;
        tick_quanta = static_cast<uint32_t>(g_animation_tick_accumulator);
        g_animation_tick_accumulator -= static_cast<float>(tick_quanta);
    }
    g_animation_scale_bits.store(float_bits(scale),
                                 std::memory_order_release);
    g_animation_tick_quanta.store(tick_quanta,
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
            std::fprintf(stderr,
                         "[ANIMATION-TIMING] samples=%u scale=%.3f..%.3f "
                         "mean=%.3f calls(don3d=%u lumen=%u)\n",
                         samples, minimum, maximum,
                         samples ? total / samples : 0.0,
                         don3d_calls, lumen_calls);
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
    g_animation_lumen_calls.fetch_add(1, std::memory_order_relaxed);
    const uint32_t tick_quanta =
        g_animation_tick_quanta.load(std::memory_order_acquire);
    ctx->fpr[1] = static_cast<double>(static_cast<float>(
        ctx->fpr[1] * static_cast<double>(tick_quanta)));
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
