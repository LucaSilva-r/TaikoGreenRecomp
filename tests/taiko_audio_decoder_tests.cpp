#include "taiko_audio_decoder.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>
#include <vector>

namespace {

#define CHECK(expression) do { if (!(expression)) std::abort(); } while (0)

void le16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void le32(std::vector<uint8_t>& out, uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

void tag(std::vector<uint8_t>& out, const char* value)
{
    out.insert(out.end(), value, value + 4);
}

void patch_le32(std::vector<uint8_t>& out, size_t offset, uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        out[offset + shift / 8] = static_cast<uint8_t>(value >> shift);
}

std::vector<uint8_t> make_pcm_riff()
{
    constexpr uint32_t rate = 44100;
    constexpr uint32_t frames = 441;
    std::vector<uint8_t> out;
    tag(out, "RIFF");
    le32(out, 0);
    tag(out, "WAVE");

    tag(out, "fmt ");
    le32(out, 16);
    le16(out, 1);
    le16(out, 2);
    le32(out, rate);
    le32(out, rate * 4);
    le16(out, 4);
    le16(out, 16);

    tag(out, "smpl");
    le32(out, 60);
    for (unsigned index = 0; index < 7; ++index) le32(out, 0);
    le32(out, 1); /* one loop */
    le32(out, 0);
    le32(out, 0); /* identifier */
    le32(out, 0); /* forward loop */
    le32(out, 100);
    le32(out, 199); /* inclusive */
    le32(out, 0);
    le32(out, 0); /* infinite */

    tag(out, "data");
    le32(out, frames * 4);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const int16_t sample = static_cast<int16_t>(
            std::sin(static_cast<double>(frame) * 0.1) * 12000.0);
        le16(out, static_cast<uint16_t>(sample));
        le16(out, static_cast<uint16_t>(-sample));
    }
    patch_le32(out, 4, static_cast<uint32_t>(out.size() - 8));
    return out;
}

} // namespace

int main()
{
    const std::vector<uint8_t> riff = make_pcm_riff();
    TaikoDecodedAudio decoded;
    std::string failure;
    CHECK(taiko_audio_decode_riff(riff, 48000, nullptr, decoded, failure));
    CHECK(decoded.pcm && decoded.sample_rate == 48000);
    const size_t frames = decoded.pcm->size() / 2;
    CHECK(frames >= 479 && frames <= 481);
    CHECK(decoded.has_loop);
    CHECK(decoded.loop_start >= 108 && decoded.loop_start <= 110);
    CHECK(decoded.loop_end >= 217 && decoded.loop_end <= 219);

    TaikoDecodedAudio cached;
    CHECK(taiko_audio_decode_riff(riff, 48000, nullptr, cached, failure));
    CHECK(cached.cache_hit);
    CHECK(cached.pcm == decoded.pcm);

    std::atomic<bool> cancelled{true};
    TaikoDecodedAudio stopped;
    /* A different output-rate key ensures this reaches the cancellation gate
     * rather than the populated 48 kHz cache entry. */
    CHECK(!taiko_audio_decode_riff(riff, 32000, &cancelled,
                                   stopped, failure));
    CHECK(failure == "cancelled");

    std::vector<uint8_t> corrupt = riff;
    corrupt[0] = 'X';
    CHECK(!taiko_audio_decode_riff(corrupt, 48000, nullptr,
                                   stopped, failure));
    CHECK(!taiko_audio_decode_song("../escape", 48000, nullptr,
                                   stopped, failure));
    CHECK(failure == "invalid music_id");

    if (const char* real_id = std::getenv("TAIKO_TEST_REAL_MUSIC_ID")) {
        TaikoDecodedAudio real_song;
        if (!taiko_audio_decode_song(real_id, 48000, nullptr,
                                     real_song, failure)) {
            std::fprintf(stderr, "real song decode failed: %s\n",
                         failure.c_str());
            std::abort();
        }
        CHECK(real_song.pcm && !real_song.pcm->empty());
        CHECK(real_song.sample_rate == 48000);
        std::fprintf(stderr, "real song cue=%zu gain=%f group=%u\n",
                     real_song.preview_start, real_song.song_gain, real_song.volume_group);
    }

    const auto nonce = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("taiko-audio-decoder-" + std::to_string(nonce));
    const std::filesystem::path directory =
        root / "data" / "sound" / "bgm" / "nub";
    std::filesystem::create_directories(directory);
    {
        std::ofstream nub(directory / "SONG_TEST.nub", std::ios::binary);
        const char nub_header[32]{};
        nub.write(nub_header, sizeof nub_header);
        nub.write(reinterpret_cast<const char*>(riff.data()),
                  static_cast<std::streamsize>(riff.size()));
    }
#ifdef _WIN32
    CHECK(_putenv_s("PS3_VFS_ROOT", root.string().c_str()) == 0);
#else
    CHECK(setenv("PS3_VFS_ROOT", root.string().c_str(), 1) == 0);
#endif
    TaikoDecodedAudio song;
    CHECK(taiko_audio_decode_song("TEST", 48000, nullptr, song, failure));
    CHECK(song.pcm == decoded.pcm);
    CHECK(taiko_audio_decode_song("test", 48000, nullptr, song, failure));
    CHECK(song.pcm == decoded.pcm);
    // Metadata belongs to a voice, never to the shared full-song PCM cache.
    std::vector<uint8_t> nsh(2048);
    const auto be = [&nsh](size_t at, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) nsh[at + i] = value >> (24 - i * 8);
    };
    be(0, 0x00020100); be(0xc, 1); be(0x18, 0x20); be(0x20, 0x30);
    be(0x30, 0x61743300); be(0xc0, 20); be(0x90, 11);
    be(0x64, 0xc0c00000); // -6 dB
    be(0xe0, 3); // 3 ms = 144 frames at 48 kHz
    CHECK(taiko_audio_apply_nsh(nsh, song));
    CHECK(song.preview_start == 144 && song.volume_group == 11);
    CHECK(std::abs(song.song_gain - 0.501187f) < 0.00001f);
    CHECK(song.pcm == decoded.pcm && decoded.preview_start == 0);
    be(0xe0, 0xffffffff); // Cue outside the song cannot escape the PCM buffer.
    CHECK(taiko_audio_apply_nsh(nsh, song) && song.preview_start == 0);
    be(0x64, 0x7fc00000);
    CHECK(!taiko_audio_apply_nsh(nsh, song));
    CHECK(song.song_gain == 1.0f && song.preview_start == 0);
    be(0x20, 0xfffffff0);
    CHECK(!taiko_audio_apply_nsh(nsh, song));
    nsh.resize(16);
    CHECK(!taiko_audio_apply_nsh(nsh, song));
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    CHECK(!cleanup_error);
    return 0;
}
