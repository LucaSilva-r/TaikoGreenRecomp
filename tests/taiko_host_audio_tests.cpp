#include "taiko_host_audio.h"
#include "cellAudio.h"
#include "taiko_menu_samples.h"

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
    taiko_host_audio_set_group_gain(11, 0.0f);
    for (unsigned i = 0; i < 20; ++i) {
        block.fill(0); captured_mixer(block.data(), CELL_AUDIO_BLOCK_SAMPLES);
    }
    const float hit[] = {0.25f, -0.25f};
    taiko_host_audio_test_install_drums(hit, 2);
    taiko_host_audio_set_group_gain(5, 0.5f);
    taiko_host_audio_play_sfx(TaikoPlusSfx::Move);
    taiko_host_audio_play_sfx(TaikoPlusSfx::Confirm);
    block.fill(0); captured_mixer(block.data(), CELL_AUDIO_BLOCK_SAMPLES);
    CHECK(block[0] == 0.25f && block[1] == 0.25f);
    CHECK(block[2] == -0.25f && block[3] == -0.25f);
    CHECK(block[4] == 0 && block.back() == 0); // No ramp or loop on hits.
    taiko_host_audio_begin_gameplay_handoff();
    taiko_host_audio_play_sfx(TaikoPlusSfx::Move);
    block.fill(0); captured_mixer(block.data(), CELL_AUDIO_BLOCK_SAMPLES);
    CHECK(block[0] == 0 && block.back() == 0);

    // Synthetic mono NUB: known filter-zero nibbles, no copyrighted data.
    std::vector<uint8_t> nub(256 + 16);
    const auto put = [&](size_t p, uint32_t v) {
        for (unsigned i = 0; i < 4; ++i) nub[p+i] = v >> (24-i*8);
    };
    put(0, 0x00020100); put(12, 1); put(16, 256); put(24, 32);
    put(32, 48); put(48, 0x76616700); put(48+20, 16);
    put(48+96, 5); put(48+188, 48000);
    nub[258] = 0xf1;
    TaikoMenuSample sample;
    CHECK(taiko_decode_menu_sample(nub, 0, sample));
    CHECK(sample.pcm.size() == 28 && sample.pcm[0] == 0.125f &&
          sample.pcm[1] == -0.125f);
    CHECK(!taiko_decode_menu_sample(nub, 1, sample));
    nub[256] = 0x50; CHECK(!taiko_decode_menu_sample(nub, 0, sample));
    nub[256] = 0; nub.pop_back();
    CHECK(!taiko_decode_menu_sample(nub, 0, sample));
    return 0;
}
