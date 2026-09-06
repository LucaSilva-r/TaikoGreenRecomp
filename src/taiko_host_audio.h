#ifndef TAIKO_HOST_AUDIO_H
#define TAIKO_HOST_AUDIO_H

#include <cstdint>
#include <cstddef>
#include <string_view>

enum class TaikoPlusSfx : uint8_t {
    Move,
    Difficulty,
    Confirm,
    Cancel,
};

/* Process-lifetime cellAudio mixer controls. These functions publish atomics
 * only and are safe to call outside the audio callback. */
void taiko_host_audio_install();
void taiko_host_audio_set_scene_active(bool active);
void taiko_host_audio_select_preview(std::string_view music_id,
                                     uint64_t generation);
void taiko_host_audio_play_sfx(TaikoPlusSfx sound);
void taiko_host_audio_begin_gameplay_handoff();
void taiko_host_audio_reacquire_menu();

#ifdef TAIKO_HOST_AUDIO_TESTING
void taiko_host_audio_test_publish_pcm(const float* stereo, size_t frames,
                                       bool loop, uint64_t generation);
#endif

#endif /* TAIKO_HOST_AUDIO_H */
