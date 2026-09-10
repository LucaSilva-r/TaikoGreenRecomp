#pragma once
#include <algorithm>
#include <cstdint>

// Decoder-side recovery, deliberately opt-in until live validation. Source
// requests include buffering jitter; establish a fixed baseline only after
// priming, using the fastest observed progress over a two-second window.
// Never move that baseline after a stall (that would forgive permanent drift).
struct TaikoAudioClock {
    uint64_t start_ns = 0, anchor_ns = 0;
    double baseline = 0;
    bool sampled = false, anchored = false;

    double correction(uint64_t now, double position, double applied_offset,
                      unsigned rate) {
        if (!rate) return 0;
        if (!start_ns) { start_ns = now; return 0; }
        const double elapsed = (now - start_ns) / 1e9;
        const double neutral = position - applied_offset;
        if (!anchored) {
            if (elapsed < 3) return 0; // Initial guest priming/prefill pause.
            const double candidate = neutral - elapsed * rate;
            if (!sampled || candidate > baseline) baseline = candidate;
            sampled = true;
            if (elapsed < 5) return 0;
            anchored = true;
            anchor_ns = now;
        }
        const double expected = elapsed * rate + baseline + applied_offset;
        const double lag = expected - position;
        // Two ATRAC blocks plus margin: don't chase normal refill jitter.
        const double tolerance = 4096.0 + rate * 0.020;
        if (lag <= tolerance) return 0;
        // Recover the accumulated delay, retaining one block of slack. A
        // future callback accounts for any further stall during recovery.
        return std::max(0.0, lag - 2048.0);
    }
};
