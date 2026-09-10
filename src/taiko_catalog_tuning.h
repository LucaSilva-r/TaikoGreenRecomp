#ifndef TAIKO_CATALOG_TUNING_H
#define TAIKO_CATALOG_TUNING_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace taiko_catalog_detail {

// Green's loader func_007D5034 locates the string pool after count * 0x90c
// records. Each record has three string offsets then 18 course records of
// 0x80 bytes (nine solo, nine duet); course +4 is the authored star count.
inline std::unordered_map<std::string, std::array<uint8_t, 5>> parse_star_ratings(
    std::string_view data)
{
    std::unordered_map<std::string, std::array<uint8_t, 5>> result;
    if (data.size() < 4) return result;
    const auto word = [&](std::size_t offset) {
        const auto* p = reinterpret_cast<const unsigned char*>(data.data() + offset);
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
               uint32_t(p[2]) << 8 | p[3];
    };
    const uint32_t count = word(0);
    constexpr std::size_t stride = 0x90c;
    if (count > (data.size() - 4) / stride) return result;
    const std::size_t strings = 4 + std::size_t(count) * stride;
    const auto string_at = [&](uint32_t offset) -> std::string_view {
        if (offset >= data.size() - strings) return {};
        const std::size_t start = strings + offset;
        const std::size_t end = data.find('\0', start);
        if (end == std::string::npos) return {};
        return std::string_view(data).substr(start, end - start);
    };
    for (uint32_t i = 0; i < count; ++i) {
        const std::size_t record = 4 + std::size_t(i) * stride;
        const std::string id(string_at(word(record)));
        if (id.empty()) continue;
        // Ura is a separate ex_<musicid> tuning entry whose Oni course is
        // installed as <musicid>_x.bin. Merge it without replacing base courses.
        const bool extra = id.compare(0, 3, "ex_") == 0;
        if (extra && id.size() == 3) continue;
        auto& ratings = result[extra ? id.substr(3) : id];
        // Match chart names rather than assuming a fixed ordinal for Ura.
        for (unsigned course = 0; course < 9; ++course) {
            const std::size_t offset = record + 12 + course * 0x80;
            const auto name = string_at(word(offset));
            const uint32_t stars = word(offset + 4);
            if (!stars || stars > 10) continue;
            static constexpr char suffixes[] = "enhmx";
            for (unsigned d = 0; d < 5; ++d)
                if (name == id + "1p_" + suffixes[d]) {
                    if (!extra) ratings[d] = stars;
                    else if (d == 3) ratings[4] = stars;
                }
        }
    }
    return result;
}

}
#endif
