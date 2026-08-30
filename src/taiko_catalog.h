#ifndef TAIKO_CATALOG_H
#define TAIKO_CATALOG_H

#include <cstddef>
#include <cstdint>
#include <string>

struct TaikoCatalogSong {
    std::string music_id;
    std::string title;
    std::string genre;
    uint32_t unique_id = 0;
    uint8_t difficulty_mask = 0;
};

/* Difficulty order matches Green's chart suffixes e/n/h/m/x. */
enum TaikoCatalogDifficulty : unsigned {
    TAIKO_DIFFICULTY_EASY = 0,
    TAIKO_DIFFICULTY_NORMAL,
    TAIKO_DIFFICULTY_HARD,
    TAIKO_DIFFICULTY_ONI,
    TAIKO_DIFFICULTY_URA,
    TAIKO_DIFFICULTY_COUNT,
};

bool taiko_catalog_load();
std::size_t taiko_catalog_count();
const TaikoCatalogSong* taiko_catalog_song(std::size_t index);
const char* taiko_catalog_difficulty_name(unsigned difficulty);

#endif
