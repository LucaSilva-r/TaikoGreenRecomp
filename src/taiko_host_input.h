#ifndef TAIKO_HOST_INPUT_H
#define TAIKO_HOST_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum taiko_host_action {
    TAIKO_ACTION_HIT_SL  = 1u << 0,
    TAIKO_ACTION_HIT_CL  = 1u << 1,
    TAIKO_ACTION_HIT_CR  = 1u << 2,
    TAIKO_ACTION_HIT_SR  = 1u << 3,
    TAIKO_ACTION_ENTER   = 1u << 4,
    TAIKO_ACTION_SERVICE = 1u << 5,
    TAIKO_ACTION_TEST    = 1u << 6,
    TAIKO_ACTION_COIN    = 1u << 7,
    TAIKO_ACTION_UP      = 1u << 8,
    TAIKO_ACTION_DOWN    = 1u << 9,
};

typedef struct taiko_host_input_snapshot {
    uint32_t levels[2];
    uint32_t rising[2];
    uint64_t hit_timestamp_ns[2][4];
    int active;
} taiko_host_input_snapshot;

typedef struct taiko_input_trace_marker {
    uint64_t sequence;
    uint64_t event_ns;
    uint64_t usio_ns;
} taiko_input_trace_marker;

void taiko_host_input_reset(void);
void taiko_host_input_set_active(int active);
void taiko_host_input_update_levels(unsigned player, uint32_t levels,
                                    uint64_t timestamp_ns);
void taiko_host_input_press(unsigned player, uint32_t actions,
                            uint64_t timestamp_ns);
void taiko_host_input_release(unsigned player, uint32_t actions);
void taiko_host_input_consume(taiko_host_input_snapshot* snapshot);
/* End-to-end diagnostic marker: USIO publishes the kernel event and guest
 * consumption times; the renderer snapshots the newest marker onto batches. */
void taiko_host_input_trace_consumed(uint64_t event_ns, uint64_t usio_ns);
void taiko_host_input_trace_snapshot(taiko_input_trace_marker* marker);

#ifdef __cplusplus
}
#endif
#endif
