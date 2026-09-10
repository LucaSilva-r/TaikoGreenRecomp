#include "taiko_animation_scale.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static int close_enough(float a, float b) { return fabsf(a - b) < 1e-5f; }

int main(void)
{
    /* 240 Hz: an ordinary frame and its jitter both snap to a quarter tick. */
    assert(close_enough(taiko_animation_snap_scale(4166666u, 240u), 0.25f));
    assert(close_enough(taiko_animation_snap_scale(3900000u, 240u), 0.25f));
    assert(close_enough(taiko_animation_snap_scale(4500000u, 240u), 0.25f));
    /* A genuinely dropped frame still steps twice. */
    assert(close_enough(taiko_animation_snap_scale(8333333u, 240u), 0.50f));
    /* Halfway between two periods is not a plausible multiple. */
    assert(taiko_animation_snap_scale(6250000u, 240u) == 0.0f);
    /* 60 Hz keeps a full authored tick. */
    assert(close_enough(taiko_animation_snap_scale(16666666u, 60u), 1.0f));
    assert(close_enough(taiko_animation_snap_scale(33333333u, 60u), 2.0f));
    /* Out of range: hitches and unknown rates fall back to the raw ratio. */
    assert(taiko_animation_snap_scale(90000000u, 60u) == 0.0f);
    assert(taiko_animation_snap_scale(4166666u, 0u) == 0.0f);
    assert(taiko_animation_snap_scale(0u, 240u) == 0.0f);
    printf("taiko_animation_scale_tests OK\n");
    return 0;
}
