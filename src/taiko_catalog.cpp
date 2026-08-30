/* Host-side view of Green's installed stock catalog.
 *
 * musicinfo.xml is the authoritative metadata/order, but it also contains
 * challenge medleys and entries whose charts are not installed. A song is
 * exposed only when at least one of its solo e/n/h/m/x chart files exists.
 */
#include "taiko_catalog.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <vector>

namespace {

std::once_flag g_once;
std::vector<TaikoCatalogSong> g_songs;
bool g_loaded = false;

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    stream.seekg(0, std::ios::end);
    const std::streamoff length = stream.tellg();
    if (length <= 0) return {};
    stream.seekg(0, std::ios::beg);
    std::string contents(static_cast<std::size_t>(length), '\0');
    stream.read(contents.data(), length);
    return stream ? contents : std::string{};
}

void replace_all(std::string& value, std::string_view from,
                 std::string_view to)
{
    std::size_t position = 0;
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
}

std::string decode_xml(std::string value)
{
    replace_all(value, "&amp;", "&");
    replace_all(value, "&lt;", "<");
    replace_all(value, "&gt;", ">");
    replace_all(value, "&quot;", "\"");
    replace_all(value, "&apos;", "'");
    return value;
}

std::string tag_value(std::string_view block, std::string_view tag)
{
    const std::string opening = "<" + std::string(tag) + ">";
    const std::string closing = "</" + std::string(tag) + ">";
    const std::size_t begin = block.find(opening);
    if (begin == std::string_view::npos) return {};
    const std::size_t value_begin = begin + opening.size();
    const std::size_t end = block.find(closing, value_begin);
    if (end == std::string_view::npos) return {};
    return decode_xml(std::string(block.substr(value_begin, end - value_begin)));
}

uint32_t parse_uint(std::string_view value)
{
    uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                        result);
    return parsed.ec == std::errc{} ? result : 0;
}

uint8_t chart_mask(const std::filesystem::path& fumen_root,
                   const std::string& music_id)
{
    static constexpr char suffixes[TAIKO_DIFFICULTY_COUNT] = {
        'e', 'n', 'h', 'm', 'x'
    };
    uint8_t mask = 0;
    const std::filesystem::path solo = fumen_root / music_id / "solo";
    for (unsigned difficulty = 0; difficulty < TAIKO_DIFFICULTY_COUNT;
         ++difficulty) {
        const std::string filename = music_id + "_" + suffixes[difficulty] +
                                     ".bin";
        std::error_code error;
        if (std::filesystem::is_regular_file(solo / filename, error))
            mask |= static_cast<uint8_t>(1u << difficulty);
    }
    return mask;
}

void load_once()
{
    const char* configured_root = std::getenv("PS3_VFS_ROOT");
    const std::filesystem::path root =
        configured_root && configured_root[0] ? configured_root : "game/vfs";
    std::filesystem::path metadata =
        root / "data/config/S11100-1/musicinfo.xml";
    std::string xml = read_file(metadata);
    if (xml.empty()) {
        metadata = root / "data/musicinfo.xml";
        xml = read_file(metadata);
    }
    if (xml.empty()) {
        std::fprintf(stderr,
                     "[taiko_catalog] could not read Green musicinfo.xml "
                     "under %s\n", root.string().c_str());
        return;
    }

    const std::filesystem::path fumen_root = root / "data/fumen";
    std::size_t position = 0;
    std::size_t metadata_count = 0;
    while ((position = xml.find("<Data", position)) != std::string::npos) {
        const std::size_t opening_end = xml.find('>', position);
        const std::size_t closing = xml.find("</Data>", opening_end);
        if (opening_end == std::string::npos || closing == std::string::npos)
            break;
        const std::string_view block(xml.data() + opening_end + 1,
                                     closing - opening_end - 1);
        position = closing + 7;
        TaikoCatalogSong song;
        song.music_id = tag_value(block, "musicid");
        if (song.music_id.empty()) continue;
        ++metadata_count;
        song.difficulty_mask = chart_mask(fumen_root, song.music_id);
        if (!song.difficulty_mask) continue;
        song.title = tag_value(block, "musicname");
        song.genre = tag_value(block, "genrename");
        song.unique_id = parse_uint(tag_value(block, "uniqueid"));
        if (song.title.empty()) song.title = song.music_id;
        g_songs.emplace_back(std::move(song));
    }

    g_loaded = !g_songs.empty();
    std::fprintf(stderr,
                 "[taiko_catalog] loaded %zu playable songs from %zu metadata "
                 "entries (%s)\n",
                 g_songs.size(), metadata_count, metadata.string().c_str());
}

} // namespace

bool taiko_catalog_load()
{
    std::call_once(g_once, load_once);
    return g_loaded;
}

std::size_t taiko_catalog_count()
{
    return taiko_catalog_load() ? g_songs.size() : 0;
}

const TaikoCatalogSong* taiko_catalog_song(std::size_t index)
{
    if (!taiko_catalog_load() || index >= g_songs.size()) return nullptr;
    return &g_songs[index];
}

const char* taiko_catalog_difficulty_name(unsigned difficulty)
{
    static constexpr const char* names[TAIKO_DIFFICULTY_COUNT] = {
        "EASY", "NORMAL", "HARD", "ONI", "URA"
    };
    return difficulty < TAIKO_DIFFICULTY_COUNT ? names[difficulty] : "?";
}
