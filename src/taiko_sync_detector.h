#pragma once
#include <cmath>
#include <cstdint>

// 48 kHz calibration tones: 1 ms energy windows, 4 kHz spectral qualification.
// Streaming state spans callback boundaries; no allocation or logging here.
class TaikoSyncDetector {
    float cosine[48]{}, sine[48]{};
    unsigned count = 0, qualified = 0;
    double energy = 0, real = 0, imaginary = 0, best = 0;
    uint64_t window_ns = 0, best_ns = 0, refractory_ns = 0;
public:
    TaikoSyncDetector() {
        for (unsigned i = 0; i < 48; ++i) {
            cosine[i] = std::cos(i * 6.283185307179586 * 4000 / 48000);
            sine[i] = std::sin(i * 6.283185307179586 * 4000 / 48000);
        }
    }
    void reset() {
        count = qualified = 0;
        energy = real = imaginary = best = 0;
        window_ns = best_ns = refractory_ns = 0;
    }
    uint64_t sample(float value, uint64_t sample_ns) {
        if (!count) window_ns = sample_ns;
        energy += value * value;
        real += value * cosine[count];
        imaginary += value * sine[count];
        if (++count != 48) return 0;
        const double power = energy / 48;
        const double fraction = energy > 0
            ? 2 * (real * real + imaginary * imaginary) / (48 * energy) : 0;
        count = 0;
        energy = real = imaginary = 0;
        if (sample_ns < refractory_ns) return 0;
        if (power > 0.00001 && fraction > 0.80) {
            ++qualified;
            if (power > best) {
                best = power;
                best_ns = window_ns + 500000;
                return 0;
            }
            if (power > best * 0.70) return 0;
        }
        const uint64_t result = qualified >= 3 ? best_ns : 0;
        best = 0;
        qualified = 0;
        if (result) refractory_ns = result + 750000000;
        return result;
    }
};
