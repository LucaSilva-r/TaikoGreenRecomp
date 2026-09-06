#include "taiko_host_audio.h"
#include "cellAudio.h"

#include <algorithm>
#include <array>
#include <vector>
#include <cmath>
#include <cstdlib>

#define CHECK(expression) do { if (!(expression)) std::abort(); } while (0)

static CellAudioExternalMixer captured_mixer;

extern "C" void cellAudioSetExternalMixer(CellAudioExternalMixer mixer)
{
    captured_mixer = mixer;
}

int main()
{
    taiko_host_audio_install();
    taiko_host_audio_set_group_gain(11, 1.0f);
    CHECK(captured_mixer);

    taiko_host_audio_set_scene_active(true);
    std::array<float, CELL_AUDIO_BLOCK_SAMPLES * 2> block;
    block.fill(1.0f);
    captured_mixer(block.data(), CELL_AUDIO_BLOCK_SAMPLES);
    CHECK(block.front() < 1.0f);
    CHECK(block.back() > 0.0f);
    for (unsigned index = 0; index < 19; ++index) {
        block.fill(1.0f);
        captured_mixer(block.data(), CELL_AUDIO_BLOCK_SAMPLES);
    }
    CHECK(block.back() == 0.0f);

    taiko_host_audio_begin_gameplay_handoff();
    block.fill(1.0f);
    captured_mixer(block.data(), CELL_AUDIO_BLOCK_SAMPLES);
    CHECK(block.front() > 0.0f);
    CHECK(block.back() > block.front());

    taiko_host_audio_set_scene_active(true);
    for (unsigned index = 0; index < 20; ++index) {
        block.fill(0.0f);
        captured_mixer(block.data(), CELL_AUDIO_BLOCK_SAMPLES);
    }

    const std::array<float, 4> positive{{0.25f, 0.25f, 0.25f, 0.25f}};
    taiko_host_audio_test_publish_pcm(
        positive.data(), positive.size() / 2, true, 20);
    block.fill(0.0f);
    captured_mixer(block.data(), CELL_AUDIO_BLOCK_SAMPLES);
    CHECK(block.front() == 0.0f);
    for (unsigned index = 0; index < 19; ++index) {
        block.fill(0.0f);
        captured_mixer(block.data(), CELL_AUDIO_BLOCK_SAMPLES);
    }
    CHECK(std::abs(block.back() - 0.25f) < 0.0001f);

    /* A decoded result older than the authoritative selection generation is
     * retired without replacing the audible voice. */
    taiko_host_audio_test_publish_pcm(
        positive.data(), positive.size() / 2, true, 19);
    block.fill(0.0f);
    captured_mixer(block.data(), CELL_AUDIO_BLOCK_SAMPLES);
    CHECK(std::abs(block.back() - 0.25f) < 0.0001f);

    const std::array<float, 4> negative{{-0.25f, -0.25f, -0.25f, -0.25f}};
    taiko_host_audio_test_publish_pcm(
        negative.data(), negative.size() / 2, true, 21);
    for (unsigned index = 0; index < 20; ++index) {
        block.fill(0.0f);
        captured_mixer(block.data(), CELL_AUDIO_BLOCK_SAMPLES);
    }
    CHECK(std::abs(block.back() + 0.25f) < 0.0001f);
    taiko_host_audio_set_group_gain(11, 0.2f);
    for (unsigned i = 0; i < 20; ++i) {
        block.fill(0.0f);
        captured_mixer(block.data(), CELL_AUDIO_BLOCK_SAMPLES);
    }
    CHECK(std::abs(block.back() + 0.05f) < 0.0001f);
    taiko_host_audio_set_group_gain(11, 0.0f);
    for (unsigned i = 0; i < 20; ++i) {
        block.fill(0.0f);
        captured_mixer(block.data(), CELL_AUDIO_BLOCK_SAMPLES);
    }
    CHECK(block.back() == 0.0f);
    std::vector<float> cued(24000, 0.5f);
    std::fill(cued.begin()+12000, cued.end(), -0.5f);
    taiko_host_audio_set_group_gain(11, 0.5f);
    taiko_host_audio_test_publish_pcm(cued.data(),12000,false,22,6000,0.5f);
    for (unsigned i = 0; i < 20; ++i) {
        block.fill(0.0f);
        captured_mixer(block.data(), CELL_AUDIO_BLOCK_SAMPLES);
    }
    CHECK(std::abs(block.back()+0.125f)<0.0001f); // Cue and both gain factors.
    return 0;
}
