#include "taiko_custom_songs.h"
#include "taiko_audio_decoder.h"
#include "taiko_chart.h"
#include <set>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace {
namespace fs = std::filesystem;
fs::path from_utf8(std::string_view text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}
std::mutex prepare_mutex;
std::string active_id, active_cache;
std::mutex assets_mutex;
bool chart_ready(const TaikoCatalogSong& song, uint32_t& lead)
{
    std::ifstream ready(from_utf8(song.custom_cache) / "ready");
    std::string revision;
    if (!(ready >> revision >> lead) || revision != song.custom_revision || lead > 60000) return false;
    uint8_t mask = 0;
    std::string course;
    std::set<std::string> seen;
    uint64_t length;
    std::string expected;
    while (ready >> course >> length >> expected) {
        if (course.empty()) return false;
        const auto d = std::string_view("enhmx").find(course[0]);
        if (d == std::string_view::npos || expected.size() != 64 || !seen.insert(course).second ||
            (course.size() != 1 && course != std::string(1,course[0])+"_1" && course != std::string(1,course[0])+"_2")) return false;
        const auto path = from_utf8(song.custom_cache) / (course + ".bin");
        std::error_code ec;
        if (fs::file_size(path, ec) != length || ec) return false;
        taiko_plus::Sha256 hash;
        if (!taiko_hash_file_sha256(utf8(path), hash)) return false;
        const char* hex = "0123456789abcdef";
        for (unsigned i = 0; i < 32; ++i)
            if (expected[i*2] != hex[hash.bytes[i] >> 4] ||
                expected[i*2+1] != hex[hash.bytes[i] & 15]) return false;
        if (course.size() == 1) mask |= 1u << d;
    }
    if (song.genre == "NIJIIRO") for (unsigned d=0; d<5; ++d) if (song.difficulty_mask & (1u<<d))
        for (const char* suffix : {"", "_1", "_2"})
            if (!seen.count(std::string(1,"enhmx"[d])+suffix)) return false;
    return ready.eof() && mask == song.difficulty_mask;
}
void put(std::vector<uint8_t>& bytes, size_t at, uint32_t value, bool big = false)
{
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = uint8_t(value >> (big ? 24 - 8*i : 8*i));
}
bool write(const fs::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    return bool(out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}
std::string asset_path(const char* guest)
{
    if (!guest) return {};
    std::string path(guest);
    const auto data = path.find("/data/");
    if (data == std::string::npos) return {};
    path.erase(0, data + 6);
    std::lock_guard<std::mutex> lock(assets_mutex);
    if (active_id.empty()) return {};
    for (const char* kind : {"solo", "duet"}) {
    const std::string base = "fumen/" + active_id + "/" + kind + "/" + active_id + "_";
    if (path.compare(0, base.size(), base) == 0) {
        const auto tail = path.substr(base.size());
        for (char course : std::string("enhmx"))
            if (tail == std::string(1, course) + ".bin" ||
                tail == std::string(1, course) + "_1.bin" ||
                tail == std::string(1, course) + "_2.bin")
            {
                const auto exact = from_utf8(active_cache) / tail;
                if (fs::is_regular_file(exact)) return utf8(exact);
                return utf8(from_utf8(active_cache) / (std::string(1, course) + ".bin"));
            }
    }
    }
    std::string upper = active_id;
    for (char& c : upper) if (c >= 'a' && c <= 'z') c -= 32;
    for (const char* ext : {"nub", "nsh"})
        if (path == "sound/bgm/" + std::string(ext) + "/SONG_" + upper + "." + ext)
            return utf8(from_utf8(active_cache) / (std::string("audio.") + ext));
    return {};
}
}

void taiko_custom_scan(std::vector<TaikoCatalogSong>& songs)
{
    try {
        const char* configured = std::getenv("TAIKO_CUSTOM_SONGS");
        const char* vfs = std::getenv("PS3_VFS_ROOT");
        const fs::path root = configured && *configured ? from_utf8(configured) :
            fs::weakly_canonical(from_utf8(vfs && *vfs ? vfs : "game/vfs") / "data").parent_path() / "custom_songs";
        fs::create_directories(root / "TJA");

        const auto cache = root / ".cache";
        fs::create_directories(cache);
        for (int source = 0; source < 3; ++source) {
        const bool lazer = source == 1, nijiiro = source == 2;
        const char* label = nijiiro ? "Nijiiro" : lazer ? "osu!lazer" : "TJA";
        if (source == 0 && fs::is_empty(root / "TJA")) continue;
        std::vector<TaikoCatalogSong> discovered;
        try {
            if (nijiiro) taiko_chart::scan_nijiiro(discovered);
            else if (lazer) taiko_chart::scan_lazer(discovered);
            else {
                std::vector<fs::path> files;
                for (const auto& entry : fs::recursive_directory_iterator(root / "TJA", fs::directory_options::skip_permission_denied)) {
                    auto ext = utf8(entry.path().extension());
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {return char(std::tolower(c));});
                    if (entry.is_regular_file() && ext == ".tja") files.push_back(entry.path());
                }
                std::sort(files.begin(), files.end());
                for (const auto& file : files) {
                    try {discovered.push_back(taiko_chart::inspect_tja(file,root / "TJA"));}
                    catch (const std::exception& e) {std::fprintf(stderr,"[custom_songs] %s: %s\n",utf8(file).c_str(),e.what());}
                }
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr,"[custom_songs] %s discovery failed: %s\n",label,e.what());
            continue;
        }
        const size_t count = discovered.size();
        for (auto& song : discovered) {
            if (std::any_of(songs.begin(), songs.end(), [&](const auto& s) {return s.music_id == song.music_id;})) continue;
            if (source == 0) song.custom_folder = utf8(fs::relative(from_utf8(song.tja_path).parent_path(),
                                                  fs::canonical(root / "TJA")));
            std::replace(song.custom_folder.begin(), song.custom_folder.end(), '\\', '/');
            if (song.custom_folder == ".") song.custom_folder.clear();
            // ESE layout: TJA/category/song/assets. Only the category is a
            // navigation folder; the song directory is an implementation detail.
            song.custom_folder = song.custom_folder.substr(0, song.custom_folder.find('/'));
            if (song.original_title.empty()) song.original_title = song.title;
            song.genre = nijiiro ? "NIJIIRO" : lazer ? "OSU! LAZER" : "CUSTOM TJA";
            song.unique_id = 0; // Custom scores must never be submitted as cabinet content.
            song.custom_cache = utf8(cache / song.music_id / song.custom_revision);
            songs.push_back(std::move(song));
        }
        std::fprintf(stderr, "[custom_songs] discovered %zu %s charts\n", count, label);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[custom_songs] discovery failed: %s\n", e.what());
    }
}

const TaikoCatalogSong* taiko_custom_find(std::string_view id)
{
    for (size_t i = 0; i < taiko_catalog_count(); ++i) {
        const auto* song = taiko_catalog_song(i);
        if (!song->tja_path.empty() && song->music_id == id) return song;
    }
    return nullptr;
}

bool taiko_custom_preview(std::string_view id, uint32_t rate, const std::atomic<bool>* stop,
                          TaikoDecodedAudio& decoded, std::string& error)
{
    const auto* song = taiko_custom_find(id);
    if (!song) { error = "custom song is unavailable"; return false; }
    if (!taiko_audio_decode_file(song->audio_path, rate, stop, decoded, error)) return false;
    const auto cue = uint64_t(song->preview_ms) * decoded.sample_rate / 1000;
    decoded.preview_start = cue < decoded.pcm->size()/2 ? size_t(cue) : 0;
    return true;
}

bool taiko_custom_prepare(const TaikoCatalogSong& song, std::string& error)
{
    std::lock_guard<std::mutex> preparation(prepare_mutex);
    try {
        uint32_t lead = 0;
        if (!chart_ready(song, lead)) {
            taiko_chart::convert(song);
            if (!chart_ready(song, lead)) {
                error = "Chart cache validation failed"; return false;
            }
        }
        TaikoDecodedAudio decoded;
        if (!taiko_audio_decode_file(song.audio_path, 48000, nullptr, decoded, error)) return false;
        if (decoded.pcm->size() + size_t(lead) * 48 * 2 > (256u * 1024u * 1024u) / sizeof(float)) {
            error = "decoded audio with lead-in exceeds 256 MiB";
            return false;
        }
        auto padded = std::make_shared<std::vector<float>>(size_t(lead) * 48 * 2, 0.0f);
        padded->insert(padded->end(), decoded.pcm->begin(), decoded.pcm->end());
        decoded.pcm = std::move(padded);
        decoded.preview_start = 0; decoded.has_loop = false;
        // Firmware-facing RIFF descriptor, not compressed audio. The host resolves
        // this exact registered byte sequence to PCM before any codec is invoked.
        std::vector<uint8_t> riff(32768, 0);
        std::memcpy(riff.data(), "RIFF", 4); put(riff, 4, riff.size()-8);
        std::memcpy(riff.data()+8, "WAVEfmt ", 8); put(riff, 16, 52);
        riff[20] = 0xfe; riff[21] = 0xff; riff[22] = 2;
        put(riff, 24, 48000); put(riff, 28, 18000);
        riff[32] = 0; riff[33] = 3; riff[36] = 34;
        put(riff, 40, 3);
        const uint8_t guid[] = {0xbf,0xaa,0x23,0xe9,0x58,0xcb,0x71,0x44,0xa1,0x19,0xff,0xfa,0x01,0xe4,0xce,0x62};
        std::copy(std::begin(guid), std::end(guid), riff.begin()+44);
        std::memcpy(riff.data()+72, "fact", 4); put(riff, 76, 12);
        put(riff, 80, decoded.pcm->size()/2);
        std::memcpy(riff.data()+92, "tcid", 4); put(riff, 96, 64);
        // Content hash distinguishes different audio revisions with the same TJA.
        std::memcpy(riff.data()+100, song.music_id.data(), song.music_id.size());
        for (unsigned i=0;i<8;++i) riff[116+i]=uint8_t(decoded.asset_hash >> (i*8));
        put(riff,124,lead);
        std::memcpy(riff.data()+164,"data",4); put(riff,168,riff.size()-172);
        std::vector<uint8_t> header(2048,0);
        const auto be = [&](size_t off,uint32_t v){put(header,off,v,true);};
        be(0,0x20100); be(8,0x9a); be(12,1); be(16,2048); be(20,riff.size());
        be(24,0x20); be(28,0x30); be(32,0x30);
        be(0x30,0x61743300); be(0x34,0x9a); be(0x3c,3); be(0x44,riff.size()-12);
        be(0x4c,0x40); be(0x60,0xffffffff); be(0x64,0); be(0x68,0xc2c60000);
        be(0x74,0x42700000); be(0x78,0x3f800000); be(0x88,0x3f800000);
        be(0x8c,0x3f800000); be(0x90,11); be(0x9c,0xc2c80000);
        be(0xa0,1000); be(0xa4,100); be(0xac,1); be(0xbc,0x3f800000);
        be(0xc0,20); be(0xd8,0x3f800000); be(0xdc,4); be(0xe0,0x2026);
        std::copy(riff.begin()+20,riff.begin()+72,header.begin()+0xec);
        std::copy(riff.begin()+80,riff.begin()+92,header.begin()+0x120);
        const auto cache = from_utf8(song.custom_cache);
        if (!write(cache/"audio.nsh",header)) {error="cannot write custom NSH";return false;}
        auto nub=header; nub.insert(nub.end(),riff.begin(),riff.end());
        if (!write(cache/"audio.nub",nub)) {error="cannot write custom NUB descriptor";return false;}
        taiko_audio_register_custom_proxy(std::move(riff),std::move(decoded),"SONG_"+song.music_id+".nub");
        std::lock_guard<std::mutex> assets(assets_mutex);
        active_id=song.music_id; active_cache=song.custom_cache;
        return true;
    } catch (const std::exception& e) {error=e.what();return false;}
}

bool taiko_custom_identity(const TaikoCatalogSong& song, unsigned difficulty,
                           taiko_plus::ContentIdentity& identity, std::string* error)
{
    std::string failure;
    // Called from the frontend preparation worker, never the audio callback.
    if (!taiko_custom_prepare(song,failure)) {if(error)*error=failure;return false;}
    taiko_plus::ContentIdentity next;
    next.game_revision="S11100-1"; next.music_id=song.music_id;
    next.unique_id=0; next.difficulty=uint8_t(difficulty);
    if (!taiko_hash_file_sha256(utf8(from_utf8(song.custom_cache)/(std::string(1,"enhmx"[difficulty])+".bin")),next.chart_hash,error) ||
        !taiko_hash_file_sha256(song.audio_path,next.audio_hash,error)) return false;
    identity=std::move(next); return true;
}

FILE* taiko_custom_open(const char* guest,uint32_t flags)
{
    if(flags != 0) return nullptr;
    const auto path=asset_path(guest);
    if (path.empty()) return nullptr;
#ifdef _WIN32
    return _wfopen(from_utf8(path).c_str(), L"rb");
#else
    return std::fopen(path.c_str(),"rb");
#endif
}
int taiko_custom_stat(const char* guest,uint64_t* size)
{
    const auto path=asset_path(guest);
    if(path.empty() || !size) return 0;
    std::error_code ec;
    const auto length=fs::file_size(from_utf8(path),ec);
    if(ec) return 0;
    *size=length; return 1;
}
