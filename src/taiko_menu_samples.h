#ifndef TAIKO_MENU_SAMPLES_H
#define TAIKO_MENU_SAMPLES_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

/* Green's mono VAG members: BE NUB index, 0xc0-byte descriptors and raw
 * 16-byte PlayStation ADPCM blocks. No decoder/file work runs in the mixer. */
struct TaikoMenuSample {
    std::vector<float> pcm;
    float gain = 1.0f;
    uint32_t group = 5;
};

inline bool taiko_decode_menu_sample(const std::vector<uint8_t>& bank,
                                      unsigned entry, TaikoMenuSample& out)
{
    out = {};
    const auto word = [&](size_t p) {
        return uint32_t(bank[p]) << 24 | uint32_t(bank[p+1]) << 16 |
               uint32_t(bank[p+2]) << 8 | uint32_t(bank[p+3]);
    };
    if (bank.size() < 32 || word(0) != 0x00020100 || entry >= word(12))
        return false;
    const uint64_t index = uint64_t(word(24)) + uint64_t(entry) * 4;
    if (index + 4 > bank.size()) return false;
    const size_t p = word(index);
    if (p > bank.size() || bank.size() - p < 192 ||
        word(p) != 0x76616700) return false;
    const uint64_t start = uint64_t(word(16)) + word(p + 24);
    const size_t bytes = word(p + 20);
    const uint32_t rate = word(p + 188), group = word(p + 96);
    uint32_t db_bits = word(p + 52);
    float db;
    std::memcpy(&db, &db_bits, sizeof(db));
    if (!bytes || bytes % 16 || bytes > 1024 * 1024 ||
        start > bank.size() || bytes > bank.size() - start ||
        rate < 8000 || rate > 96000 || group >= 68 ||
        !std::isfinite(db) || db < -100 || db > 24) return false;
    constexpr int coefficients[5][2] = {{0,0},{60,0},{115,-52},{98,-55},{122,-60}};
    std::vector<float> mono;
    mono.reserve(bytes / 16 * 28);
    int h1 = 0, h2 = 0;
    for (size_t offset = size_t(start); offset < start + bytes; offset += 16) {
        const unsigned filter = bank[offset] >> 4, shift = bank[offset] & 15;
        if (filter >= 5) return false;
        for (unsigned n = 0; n < 28; ++n) {
            int nibble = (bank[offset + 2 + n / 2] >> ((n % 2) * 4)) & 15;
            if (nibble >= 8) nibble -= 16;
            int sample = 0;
            if ((bank[offset + 1] & 7) != 7)
                sample = int((nibble * 4096 >> shift) +
                    (int64_t(h1) * coefficients[filter][0] +
                     int64_t(h2) * coefficients[filter][1]) / 64);
            h2 = h1;
            h1 = sample;
            mono.push_back(std::clamp(sample, -32768, 32767) / 32768.0f);
        }
    }
    const size_t frames = (uint64_t(mono.size()) * 48000 + rate - 1) / rate;
    out.pcm.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        const double position = double(i) * rate / 48000;
        const size_t a = std::min(size_t(position), mono.size() - 1);
        const size_t b = std::min(a + 1, mono.size() - 1);
        out.pcm[i] = mono[a] + float(position - a) * (mono[b] - mono[a]);
    }
    out.gain = std::pow(10.0f, db / 20.0f);
    out.group = group;
    return true;
}
#endif
