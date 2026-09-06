#ifndef TAIKO_AUDIO_DECODER_H
#define TAIKO_AUDIO_DECODER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct TaikoDecodedAudio {
    std::shared_ptr<std::vector<float>> pcm;
    uint32_t sample_rate = 0;
    size_t loop_start = 0;
    size_t loop_end = 0;
    bool has_loop = false;
    bool cache_hit = false;
    uint64_t asset_hash = 0;
};

/* Resolve a RIFF prefix supplied through cellAtrac back to its complete NUB
 * member. This operation performs file I/O and must not run on an audio
 * callback. */
bool taiko_audio_resolve_riff(uint64_t prefix_hash,
                              const std::vector<uint8_t>& prefix,
                              std::vector<uint8_t>& riff,
                              std::string& source,
                              std::string& failure);

/* Decode and cache one complete RIFF. output_rate == 0 retains the encoded
 * rate; host voices request 48000 while the guest cellAtrac shim retains its
 * source-rate contract. */
bool taiko_audio_decode_riff(const std::vector<uint8_t>& riff,
                             uint32_t output_rate,
                             const std::atomic<bool>* cancelled,
                             TaikoDecodedAudio& decoded,
                             std::string& failure);

/* Resolve stock Green audio directly as SONG_<music_id>.nub and decode its
 * first RIFF member. music_id is validated before it becomes a path. */
bool taiko_audio_decode_song(std::string_view music_id,
                             uint32_t output_rate,
                             const std::atomic<bool>* cancelled,
                             TaikoDecodedAudio& decoded,
                             std::string& failure);

uint64_t taiko_audio_hash_bytes(const std::vector<uint8_t>& bytes);

#endif /* TAIKO_AUDIO_DECODER_H */
