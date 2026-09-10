#include "audio_sink.h"
#include "cellAudio.h"
#include <ps3emu/host_platform.h>
#include <ps3emu/host_sdl.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

static atomic_uint pull_blocks;
static void pull_silence(float* output)
{
    memset(output, 0, CELL_AUDIO_BLOCK_SAMPLES * 2u * sizeof(float));
    atomic_fetch_add(&pull_blocks, 1);
}

int main(void)
{
    /* Pin the prebuffer so the queue ceiling below is a fixed number rather
     * than tracking the shipping default, which is chosen for device jitter
     * margin, not for this test. What is under test is that blocks are paced
     * by device consumption instead of accepted at producer speed. */
    setenv("TAIKO_AUDIO_PREBUFFER_BLOCKS", "4", 1);

    if (ps3_host_sdl_init(PS3_HOST_SDL_AUDIO) != 0) return 77;
    if (audio_sink_init() != AUDIO_SINK_INIT_OK) {
        audio_sink_shutdown();
        ps3_host_sdl_shutdown();
        return 77;
    }

    float block[CELL_AUDIO_BLOCK_SAMPLES * 2] = {0};
    if (getenv("TAIKO_TEST_AUDIO_PULL")) {
        if (!audio_sink_start_pull(pull_silence)) return 1;
        ps3_host_sleep_ms(1000);
        const unsigned before = atomic_load(&pull_blocks);
        const unsigned queued = audio_sink_queued_frames();
        const unsigned prebuffer = audio_sink_prebuffer_frames();
        audio_sink_shutdown();
        const unsigned stopped = atomic_load(&pull_blocks);
        ps3_host_sleep_ms(30);
        const unsigned after = atomic_load(&pull_blocks);
        ps3_host_sdl_shutdown();
        printf("SDL pull smoke: blocks=%u queued=%u prebuffer=%u stopped=%u after=%u\n",
               before, queued, prebuffer, stopped, after);
        return before >= 175 && before <= 200 && queued <= CELL_AUDIO_BLOCK_SAMPLES &&
               prebuffer == 0 && stopped == after ? 0 : 1;
    }
    volatile int running = 1;
    const uint64_t start = ps3_host_monotonic_ns();
    uint32_t maximum_queued = 0;
    for (unsigned i = 0; i < 40; ++i) {
        if (!audio_sink_wait_for_block(CELL_AUDIO_BLOCK_SAMPLES, &running) ||
            !audio_sink_submit(block, CELL_AUDIO_BLOCK_SAMPLES)) {
            fprintf(stderr, "SDL3 audio sink rejected block %u\n", i);
            audio_sink_shutdown();
            ps3_host_sdl_shutdown();
            return 1;
        }
        uint32_t queued = audio_sink_queued_frames();
        if (queued > maximum_queued) maximum_queued = queued;
        if (queued > 4u * CELL_AUDIO_BLOCK_SAMPLES) {
            fprintf(stderr, "SDL3 audio queue exceeded four blocks: %u frames\n",
                    queued);
            audio_sink_shutdown();
            ps3_host_sdl_shutdown();
            return 1;
        }
    }
    const uint64_t elapsed = ps3_host_monotonic_ns() - start;
    audio_sink_shutdown();
    ps3_host_sdl_shutdown();

    /* The first four blocks are a burst prebuffer. The other 36 must be paced
     * by device consumption instead of being accepted in a producer-speed
     * loop (36 blocks are 192 ms at 48 kHz). Keep generous CI bounds. */
    if (elapsed < 100000000u || elapsed > 2000000000u) {
        fprintf(stderr, "SDL3 audio pacing took %.3f seconds\n",
                (double)elapsed / 1000000000.0);
        return 1;
    }
    printf("SDL3 audio sink smoke passed: %.3f seconds, max queue %u frames\n",
           (double)elapsed / 1000000000.0, maximum_queued);
    return 0;
}
