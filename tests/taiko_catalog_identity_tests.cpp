#include "taiko_catalog.h"
#include "taiko_catalog_tuning.h"

#undef NDEBUG
#include <cassert>
#include <filesystem>
#include <fstream>

int main()
{
    // Synthetic Green tuning record: sparse courses, reordered Ura, and
    // malformed offsets must not invent levels or read beyond the string pool.
    std::string tuning(4 + 0x90c, char(0xff));
    const auto put_word = [&](size_t at, uint32_t value) {
        for (unsigned b = 0; b < 4; ++b) tuning[at + b] = char(value >> (24 - b * 8));
    };
    const auto add_string = [&](const char* text) {
        const auto offset = uint32_t(tuning.size() - (4 + 0x90c));
        tuning.append(text).push_back('\0');
        return offset;
    };
    put_word(0, 1);
    put_word(4, add_string("fixture"));
    put_word(4 + 12, add_string("fixture1p_e"));
    put_word(4 + 16, 2);
    put_word(4 + 12 + 3 * 128, add_string("fixture1p_m"));
    put_word(4 + 16 + 3 * 128, 8);
    put_word(4 + 12 + 7 * 128, add_string("fixture1p_x"));
    put_word(4 + 16 + 7 * 128, 10);
    auto ratings = taiko_catalog_detail::parse_star_ratings(tuning);
    assert(ratings.at("fixture") == (std::array<uint8_t, 5>{2, 0, 0, 8, 10}));
    const auto base_tuning = tuning;
    put_word(4, add_string("ex_fixture"));
    put_word(4 + 12 + 3 * 128, add_string("ex_fixture1p_m"));
    ratings = taiko_catalog_detail::parse_star_ratings(tuning);
    assert(ratings.at("fixture") == (std::array<uint8_t, 5>{0, 0, 0, 0, 8}));
    tuning = base_tuning;
    put_word(4 + 16, 11); // Invalid star counts remain unknown.
    put_word(4 + 12 + 7 * 128, 0xfffffffe);
    ratings = taiko_catalog_detail::parse_star_ratings(tuning);
    assert(ratings.at("fixture") == (std::array<uint8_t, 5>{0, 0, 0, 8, 0}));
    put_word(4, uint32_t(tuning.size() - (4 + 0x90c) - 1));
    assert(taiko_catalog_detail::parse_star_ratings(tuning).empty());
    const auto last_name = uint32_t(tuning.find("fixture1p_x") - (4 + 0x90c));
    tuning.pop_back(); // Unterminated final name.
    put_word(4, last_name);
    assert(taiko_catalog_detail::parse_star_ratings(tuning).empty());
    put_word(0, 0xffffffff);
    assert(taiko_catalog_detail::parse_star_ratings(tuning).empty());
    assert(taiko_catalog_detail::parse_star_ratings("bad").empty());

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "taiko_catalog_identity_tests";
    std::filesystem::create_directories(root);
    const std::filesystem::path file = root / "abc.bin";
    {
        std::ofstream output(file, std::ios::binary);
        output << "abc";
    }
    taiko_plus::Sha256 hash;
    std::string error;
    assert(taiko_hash_file_sha256(file.string(), hash, &error));
    static constexpr uint8_t expected[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    for (std::size_t index = 0; index < hash.bytes.size(); ++index)
        assert(hash.bytes[index] == expected[index]);
    assert(!hash.empty());
    assert(!taiko_hash_file_sha256((root / "missing").string(), hash,
                                   &error));
    assert(hash.empty());
    std::filesystem::remove(file);
    std::filesystem::remove(root);
    return 0;
}
