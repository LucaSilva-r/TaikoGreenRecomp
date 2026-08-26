#ifndef TAIKO_AUDIO_OFFSET_H
#define TAIKO_AUDIO_OFFSET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Gameplay-only latency compensation in milliseconds. Positive values advance
 * audible SONG_* music against the chart. Active songs slew smoothly to a new
 * value; newly opened songs begin at that offset. */
int taiko_audio_offset_get_ms(void);

/* Clamp to 0..1000, persist immediately, and return the new value. */
int taiko_audio_offset_adjust_ms(int delta_ms);
int taiko_audio_offset_set_ms(int value_ms);

#ifdef __cplusplus
}
#endif

#endif
