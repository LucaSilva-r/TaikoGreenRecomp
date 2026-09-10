#pragma once
#include "taiko_catalog.h"
#include <filesystem>
#include <vector>

// Native chart discovery/conversion. Audio stays in its original library.
namespace taiko_chart {
std::filesystem::path path(std::string_view text);
std::string utf8(const std::filesystem::path& path);
std::string read(const std::filesystem::path& path);
std::string hash(std::string_view bytes);
std::string revision(std::string_view bytes, int osu_level = 0);
TaikoCatalogSong inspect_tja(const std::filesystem::path& source,
                             const std::filesystem::path& root);
TaikoCatalogSong inspect_osu(const std::filesystem::path& source,
                             const std::filesystem::path& audio,
                             const std::string& chart_hash,
                             const std::string& set_id, double rating);
void convert(const TaikoCatalogSong& song);
void scan_lazer(std::vector<TaikoCatalogSong>& songs);
void scan_nijiiro(std::vector<TaikoCatalogSong>& songs);
void convert_nijiiro(const TaikoCatalogSong& song);
// Validate the native binary layout and return Green byte order, preserving
// authored fields. earliest receives the earliest note time before padding.
std::string nijiiro_fumen(std::string bytes, unsigned lead, double& earliest);
}
