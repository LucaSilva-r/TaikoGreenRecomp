#ifndef TAIKO_CATALOG_H
#define TAIKO_CATALOG_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "taiko_plus_runtime.h"

struct TaikoCatalogSong {
    std::string music_id;
    std::string original_title;
    std::string title;
    std::string genre;
    uint32_t unique_id = 0;
    uint8_t difficulty_mask = 0;
    std::array<uint8_t, 5> stars{}; /* Authored Green ratings; zero means unknown. */
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
const char* taiko_catalog_genre_name(const std::string& genre);

/* Resolve and hash the exact stock chart/audio pair used by a launch. */
bool taiko_catalog_content_identity(std::size_t index, unsigned difficulty,
                                    taiko_plus::ContentIdentity& identity,
                                    std::string* error = nullptr);
bool taiko_hash_file_sha256(const std::string& path,
                            taiko_plus::Sha256& hash,
                            std::string* error = nullptr);

#endif
