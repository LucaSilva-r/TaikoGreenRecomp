#include "taiko_custom_songs.h"
#include "taiko_audio_decoder.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#ifdef _WIN32
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
extern char** environ;
#endif

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
std::string tool_path()
{
    if (const char* p = std::getenv("TAIKO_CUSTOM_TOOL")) return p;
    fs::path executable;
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (n && n < buffer.size()) executable = std::wstring(buffer.data(), n);
#else
    std::array<char, 4096> buffer{};
    const ssize_t n = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (n > 0 && size_t(n) < buffer.size()) executable = std::string(buffer.data(), n);
#endif
    if (!executable.empty()) {
        const auto bundled = executable.parent_path() / "tools/custom_songs.py";
        if (fs::is_regular_file(bundled)) return utf8(bundled);
    }
    if (fs::is_regular_file("tools/custom_songs.py")) return "tools/custom_songs.py";
    return TAIKO_CUSTOM_TOOL_DEFAULT;
}
int run_python(std::vector<std::string> args)
{
    const char* python = std::getenv("TAIKO_PYTHON");
    args.insert(args.begin(), tool_path());
    args.insert(args.begin(), python && *python ? python :
#ifdef _WIN32
        "python"
#else
        "python3"
#endif
    );
#ifdef _WIN32
    std::vector<std::wstring> wide;
    for (const auto& arg : args) wide.push_back(from_utf8(arg).wstring());
    // spawn joins argv with spaces. Quote for the Windows CRT argument parser,
    // including trailing backslashes and literal quotes; no command shell.
    std::vector<std::wstring> quoted;
    for (const auto& arg : wide) {
        std::wstring q(1, L'"');
        size_t slashes = 0;
        for (wchar_t c : arg) {
            if (c == L'\\') { ++slashes; continue; }
            q.append(slashes * (c == L'"' ? 2 : 1), L'\\');
            slashes = 0;
            if (c == L'"') q += L'\\';
            q += c;
        }
        q.append(slashes * 2, L'\\');
        q += L'"';
        quoted.push_back(std::move(q));
    }
    std::vector<const wchar_t*> argv;
    for (const auto& arg : quoted) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    return static_cast<int>(_wspawnvp(_P_WAIT, wide[0].c_str(), argv.data()));
#else
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ)) return -1;
    int status;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}
uint32_t word(std::istream& in)
{
    unsigned char b[4]{};
    in.read(reinterpret_cast<char*>(b), 4);
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}
std::string string(std::istream& in)
{
    const auto length = word(in);
    if (length > 65536) { in.setstate(std::ios::failbit); return {}; }
    std::string out(length, '\0');
    in.read(out.data(), length);
    return out;
}
bool chart_ready(const TaikoCatalogSong& song, uint32_t& lead)
{
    std::ifstream ready(from_utf8(song.custom_cache) / "ready");
    std::string revision;
    if (!(ready >> revision >> lead) || revision != song.custom_revision || lead > 60000) return false;
    uint8_t mask = 0;
    char course;
    uint64_t length;
    std::string expected;
    while (ready >> course >> length >> expected) {
        const auto d = std::string_view("enhmx").find(course);
        if (d == std::string_view::npos || expected.size() != 64 || (mask & (1u << d))) return false;
        const auto path = from_utf8(song.custom_cache) / (std::string(1, course) + ".bin");
        std::error_code ec;
        if (fs::file_size(path, ec) != length || ec) return false;
        taiko_plus::Sha256 hash;
        if (!taiko_hash_file_sha256(utf8(path), hash)) return false;
        const char* hex = "0123456789abcdef";
        for (unsigned i = 0; i < 32; ++i)
            if (expected[i*2] != hex[hash.bytes[i] >> 4] ||
                expected[i*2+1] != hex[hash.bytes[i] & 15]) return false;
        mask |= 1u << d;
    }
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
                return utf8(from_utf8(active_cache) / (std::string(1, course) + ".bin"));
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
        if (fs::is_empty(root / "TJA")) return;
        const auto cache = root / ".cache";
        fs::create_directories(cache);
        const auto index = cache / "index.bin";
        if (run_python({"scan", utf8(root / "TJA"), utf8(index)})) {
            std::fprintf(stderr, "[custom_songs] discovery failed; check Python 3.10+ and TAIKO_CUSTOM_TOOL\n");
            return;
        }
        std::ifstream in(index, std::ios::binary);
        if (word(in) != 0x32434a54) return;
        const uint32_t count = word(in);
        if (count > 100000) return;
        for (uint32_t i = 0; i < count; ++i) {
            TaikoCatalogSong song;
            song.music_id = string(in); song.title = string(in);
            song.custom_subtitle = string(in);
            song.tja_path = string(in); song.audio_path = string(in);
            song.custom_revision = string(in);
            in.read(reinterpret_cast<char*>(song.stars.data()), 5);
            song.difficulty_mask = uint8_t(in.get());
            song.preview_ms = word(in);
            if (!in || song.music_id.size() != 14 || song.music_id.substr(0, 2) != "tc" ||
                song.music_id.find_first_not_of("tc0123456789abcdef") != std::string::npos) break;
            if (std::any_of(songs.begin(), songs.end(), [&](const auto& s) {return s.music_id == song.music_id;})) continue;
            song.custom_folder = utf8(fs::relative(from_utf8(song.tja_path).parent_path(),
                                                  fs::canonical(root / "TJA")));
            std::replace(song.custom_folder.begin(), song.custom_folder.end(), '\\', '/');
            if (song.custom_folder == ".") song.custom_folder.clear();
            song.original_title = song.title;
            song.genre = "CUSTOM TJA";
            song.unique_id = 0; // Custom scores must never be submitted as cabinet content.
            song.custom_cache = utf8(cache / song.music_id / song.custom_revision);
            songs.push_back(std::move(song));
        }
        std::fprintf(stderr, "[custom_songs] discovered %u TJA songs\n", count);
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
            if (run_python({"convert", song.tja_path, song.custom_cache, song.custom_revision}) ||
                !chart_ready(song, lead)) {
                error = "TJA conversion failed; see custom_songs log"; return false;
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
