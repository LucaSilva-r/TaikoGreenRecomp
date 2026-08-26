#include "taiko_host_input.h"

#include <atomic>
#include <cstring>

namespace {
std::atomic<uint32_t> s_levels[2];
std::atomic<uint32_t> s_rising[2];
std::atomic<uint64_t> s_hit_timestamp[2][4];
std::atomic<bool> s_active;
std::atomic<uint64_t> s_trace_event_ns;
std::atomic<uint64_t> s_trace_usio_ns;
std::atomic<uint64_t> s_trace_sequence;
constexpr uint32_t kHitBits[4] = {
    TAIKO_ACTION_HIT_SL, TAIKO_ACTION_HIT_CL,
    TAIKO_ACTION_HIT_CR, TAIKO_ACTION_HIT_SR};

void latch(unsigned player, uint32_t rising, uint64_t timestamp_ns)
{
    if (player >= 2 || !rising) return;
    for (unsigned hit = 0; hit < 4; ++hit)
        if (rising & kHitBits[hit])
            s_hit_timestamp[player][hit].store(timestamp_ns,
                                                std::memory_order_release);
    s_rising[player].fetch_or(rising, std::memory_order_release);
}
}

extern "C" void taiko_host_input_reset(void)
{
    s_active.store(false, std::memory_order_release);
    s_trace_event_ns.store(0, std::memory_order_relaxed);
    s_trace_usio_ns.store(0, std::memory_order_relaxed);
    s_trace_sequence.store(0, std::memory_order_release);
    for (unsigned player = 0; player < 2; ++player) {
        s_levels[player].store(0, std::memory_order_relaxed);
        s_rising[player].store(0, std::memory_order_relaxed);
        for (unsigned hit = 0; hit < 4; ++hit)
            s_hit_timestamp[player][hit].store(0, std::memory_order_relaxed);
    }
}

extern "C" void taiko_host_input_set_active(int active)
{
    s_active.store(active != 0, std::memory_order_release);
}

extern "C" void taiko_host_input_update_levels(unsigned player, uint32_t levels,
                                                  uint64_t timestamp_ns)
{
    if (player >= 2) return;
    uint32_t previous = s_levels[player].exchange(levels,
                                                   std::memory_order_acq_rel);
    latch(player, levels & ~previous, timestamp_ns);
}

extern "C" void taiko_host_input_press(unsigned player, uint32_t actions,
                                         uint64_t timestamp_ns)
{
    if (player >= 2) return;
    uint32_t previous = s_levels[player].fetch_or(actions,
                                                   std::memory_order_acq_rel);
    latch(player, actions & ~previous, timestamp_ns);
}

extern "C" void taiko_host_input_release(unsigned player, uint32_t actions)
{
    if (player < 2)
        s_levels[player].fetch_and(~actions, std::memory_order_acq_rel);
}

extern "C" void taiko_host_input_consume(taiko_host_input_snapshot* snapshot)
{
    if (!snapshot) return;
    std::memset(snapshot, 0, sizeof(*snapshot));
    snapshot->active = s_active.load(std::memory_order_acquire) ? 1 : 0;
    for (unsigned player = 0; player < 2; ++player) {
        snapshot->levels[player] = s_levels[player].load(std::memory_order_acquire);
        snapshot->rising[player] = s_rising[player].exchange(
            0, std::memory_order_acq_rel);
        for (unsigned hit = 0; hit < 4; ++hit) {
            if (snapshot->rising[player] & kHitBits[hit])
                snapshot->hit_timestamp_ns[player][hit] =
                    s_hit_timestamp[player][hit].exchange(
                        0, std::memory_order_acq_rel);
        }
    }
}

extern "C" void taiko_host_input_trace_consumed(uint64_t event_ns,
                                                   uint64_t usio_ns)
{
    if (!event_ns || !usio_ns) return;
    const uint64_t sequence = s_trace_sequence.load(std::memory_order_relaxed);
    s_trace_sequence.store(sequence + 1u, std::memory_order_release);
    s_trace_event_ns.store(event_ns, std::memory_order_relaxed);
    s_trace_usio_ns.store(usio_ns, std::memory_order_relaxed);
    s_trace_sequence.store(sequence + 2u, std::memory_order_release);
}

extern "C" void taiko_host_input_trace_snapshot(
    taiko_input_trace_marker* marker)
{
    if (!marker) return;
    for (;;) {
        const uint64_t before =
            s_trace_sequence.load(std::memory_order_acquire);
        if (before & 1u) continue;
        const uint64_t event_ns =
            s_trace_event_ns.load(std::memory_order_relaxed);
        const uint64_t usio_ns =
            s_trace_usio_ns.load(std::memory_order_relaxed);
        const uint64_t after =
            s_trace_sequence.load(std::memory_order_acquire);
        if (before != after) continue;
        marker->sequence = after / 2u;
        marker->event_ns = event_ns;
        marker->usio_ns = usio_ns;
        return;
    }
}
