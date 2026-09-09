#include "taiko_audio_clock.h"
#include <cstdio>
#include <cmath>

int main() {
    for (unsigned rate : {44100u, 48000u}) {
        TaikoAudioClock clock;
        double position = 0, offset = rate * .105;
        const uint64_t origin = 1000000000;
        clock.correction(origin, offset, offset, rate);
        // Initial 1.93 s buffering pause; normal requests have bounded jitter.
        for (int i = 0; i < 300; ++i) {
            position = i * 2048.0;
            double t = 1.93 + position / rate + (i % 3) * .002;
            if (clock.correction(origin + uint64_t(t * 1e9), position + offset,
                                 offset, rate) != 0) return 1;
        }
        if (!clock.anchored) return 2;
        double t = 1.93 + position / rate + .5;
        const double skip = clock.correction(origin + uint64_t(t * 1e9),
                                             position + offset, offset, rate);
        if (std::abs(skip - (rate * .5 - 2048)) > 2) return 3;
        position += skip;
        // Offset adjustment must not look like drift or undo recovery.
        offset += rate * .025;
        if (clock.correction(origin + uint64_t(t * 1e9), position + offset,
                             offset, rate) != 0) return 4;
        // A second independent stall must still use the original anchor.
        t += 2;
        const double next = clock.correction(origin + uint64_t(t * 1e9),
                                             position + offset, offset, rate);
        if (std::abs(next - rate * 2) > 2) return 5;
    }
    std::puts("audio clock: priming, jitter, repeated stalls and offset changes passed");
}
