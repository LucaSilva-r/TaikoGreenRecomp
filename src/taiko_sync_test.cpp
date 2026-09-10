#include "taiko_sync_test.h"
#include "taiko_sync_detector.h"
#include "taiko_audio_offset.h"
#include "ppu_recomp.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
std::atomic<uint32_t> active_handle{0};
std::atomic<uint64_t> generation{0}, pending_ns{0};
std::atomic<unsigned> collisions{0}, unsupported{0};
// Only the SDL postmix callback owns these non-atomic objects.
TaikoSyncDetector detector;
uint64_t seen_generation = 0;
unsigned hit_count = 0; // Only the guest USIO consumer owns this counter.
}

void taiko_sync_test_start(uint32_t handle, const char* source)
{
    const char* enabled = std::getenv("TAIKO_SYNC_AUTO_HIT");
    const char* expected = std::getenv("TAIKO_SYNC_TEST_NUB_SOURCE");
    if (!enabled || std::strcmp(enabled, "1") || !expected ||
        !source || std::strcmp(source, expected)) return;
    active_handle.store(0, std::memory_order_release);
    pending_ns.store(0, std::memory_order_release);
    generation.fetch_add(1, std::memory_order_acq_rel);
    active_handle.store(handle, std::memory_order_release);
    std::fprintf(stderr, "[sync-test] armed handle=%08X; 4kHz peaks -> P1 centre; "
                 "reference=SDL postmix + one device period (estimated output time)\n",
                 handle);
}

void taiko_sync_test_stop(uint32_t handle)
{
    uint32_t expected = handle;
    if (active_handle.compare_exchange_strong(expected, 0,
                                               std::memory_order_acq_rel)) {
        generation.fetch_add(1, std::memory_order_acq_rel);
        pending_ns.store(0, std::memory_order_release);
    }
}

// Called on SDL's device thread, after stream conversion/mixing. Never waits,
// allocates, prints, reads guest memory, or invokes guest/frontend functions.
extern "C" void taiko_sync_test_postmix(const float* samples, unsigned frames,
                                        unsigned rate, unsigned channels,
                                        uint64_t callback_ns)
{
    const uint64_t current = generation.load(std::memory_order_acquire);
    if (seen_generation != current) {
        detector.reset();
        seen_generation = current;
    }
    if (!active_handle.load(std::memory_order_acquire)) return;
    if (rate != 48000 || !channels) {
        unsupported.store(rate, std::memory_order_relaxed);
        return;
    }
    const uint64_t base = callback_ns + uint64_t(frames) * 1000000000 / rate;
    for (unsigned i = 0; i < frames; ++i) {
        float mono = 0;
        for (unsigned c = 0; c < channels; ++c)
            mono += samples[i * channels + c];
        const uint64_t peak = detector.sample(mono / channels,
            base + uint64_t(i) * 1000000000 / rate);
        if (peak && generation.load(std::memory_order_acquire) == current &&
            active_handle.load(std::memory_order_acquire)) {
            uint64_t empty = 0;
            if (!pending_ns.compare_exchange_strong(empty, peak,
                                                     std::memory_order_release))
                collisions.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

uint64_t taiko_sync_test_consume(uint64_t now_ns)
{
    if (!active_handle.load(std::memory_order_acquire)) return 0;
    if (const unsigned rate = unsupported.exchange(0))
        std::fprintf(stderr, "[sync-test] unsupported device rate=%u; no auto hits\n", rate);
    uint64_t due = pending_ns.load(std::memory_order_acquire);
    if (!due || now_ns < due) return 0;
    if (!pending_ns.compare_exchange_strong(due, 0, std::memory_order_acq_rel))
        return 0;
    std::fprintf(stderr, "[sync-hit] n=%u peak_ns=%llu usio_ns=%llu late_ms=%.3f "
                 "offset_ms=%d missed_peaks=%u\n", ++hit_count,
                 static_cast<unsigned long long>(due),
                 static_cast<unsigned long long>(now_ns),
                 (now_ns - due) / 1000000.0, taiko_audio_offset_get_ms(),
                 collisions.load(std::memory_order_relaxed));
    return due;
}

extern "C" void taiko_guest_clock_trace(ppu_context* ctx, uint64_t now_us)
{
    static const bool enabled = [] {
        const char* value = std::getenv("TAIKO_GUEST_CLOCK_TRACE");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (!enabled || ctx->lr != 0x0035CA20) return;
    // Exact stopwatch caller: 0025B6A8. At this HLE boundary r31 is
    // the stopwatch object; r31 saved on its stack is the reference in ms.
    if (vm_read64(ctx->gpr[1] + 0x90) != 0x0025B714) return;
    const uint32_t timer = static_cast<uint32_t>(ctx->gpr[31]);
    if (!timer || timer > 0xffff0000u) return;
    static thread_local uint64_t previous_us = 0, report_us = 0;
    static thread_local uint32_t previous_timer = 0;
    const uint64_t gap = previous_us ? now_us - previous_us : 0;
    previous_us = now_us;
    if (timer == previous_timer && now_us < report_us && gap < 30000) return;
    previous_timer = timer;
    report_us = now_us + 250000;
    const auto field = [timer](unsigned offset) {
        return static_cast<int64_t>(vm_read64(timer + offset));
    };
    const auto real = [timer](unsigned offset) {
        uint64_t bits = vm_read64(timer + offset);
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    std::fprintf(stderr, "[guest-clock] host_us=%llu timer=%08X gap_ms=%.3f "
        "reference_ms=%d previous_reference_ms=%.3f position_ms=%.3f "
        "bias_ms=%.3f elapsed_ms=%.3f scale=%.9f/%.9f updates=%lld\n",
        static_cast<unsigned long long>(now_us), timer, gap / 1000.0,
        static_cast<int32_t>(vm_read64(ctx->gpr[1] + 0x78)),
        field(0x28) / 1000.0, field(0x30) / 1000.0,
        (field(0x18) + field(0x38)) / 1000.0,
        (static_cast<int64_t>(now_us) - field(0) - field(0x10)) / 1000.0,
        real(0x40), real(0x50), static_cast<long long>(field(0x20)));
}
