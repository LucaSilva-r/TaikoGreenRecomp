#ifndef PS3RECOMP_AUDIO_SINK_H
#define PS3RECOMP_AUDIO_SINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AUDIO_SINK_INIT_OK = 0,
    AUDIO_SINK_INIT_NULL_CLOCK = 1,
    AUDIO_SINK_INIT_FAILED = -1,
};

int audio_sink_init(void);
void audio_sink_shutdown(void);
int audio_sink_wait_for_block(uint32_t frames, const volatile int* running);
int audio_sink_submit(const float* stereo_samples, uint32_t frames);
uint32_t audio_sink_queued_frames(void);
/* Nominal host-device period/buffer capacity. This is separate from queued
 * stream data: together they bound the software-visible output latency. */
uint32_t audio_sink_device_buffer_frames(void);
const char* audio_sink_name(void);

#ifdef __cplusplus
}
#endif

#endif
