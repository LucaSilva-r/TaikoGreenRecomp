#ifndef TAIKO_ANIMATION_SCALE_H
#define TAIKO_ANIMATION_SCALE_H

#include <stdint.h>

/* Convert one guest flip interval into authored 60 Hz units.
 *
 * The guest issues its flip command from a scheduled thread, so the measured
 * interval carries about a millisecond of jitter.  That is a few percent of a
 * 60 Hz frame but a quarter of a 240 Hz one, and feeding it straight into the
 * animation step made scrolling notes shimmer even while presentation was
 * exactly vsync locked.  Snap the interval to whole vblank periods instead: a
 * normal frame is one period, a genuinely dropped frame two, and the long-run
 * rate is unchanged.
 *
 * Returns 0 when the interval is not a plausible multiple (a hitch, a rate
 * change, an unknown vblank rate); the caller then uses the raw ratio. */
static inline float taiko_animation_snap_scale(uint64_t delta_ns,
                                               unsigned vblank_hz)
{
    if (vblank_hz < 30u || vblank_hz > 1000u || !delta_ns) return 0.0f;
    const uint64_t period = 1000000000ull / vblank_hz;
    const uint64_t steps = (delta_ns + period / 2u) / period;
    if (steps < 1u || steps > 4u) return 0.0f;
    const uint64_t ideal = steps * period;
    const uint64_t error = delta_ns > ideal ? delta_ns - ideal : ideal - delta_ns;
    /* ponytail: a third of a period of tolerance; wider and two real frames
     * would merge, narrower and ordinary scheduling noise falls through. */
    if (error * 3u > period) return 0.0f;
    return (float)steps * (60.0f / (float)vblank_hz);
}

#endif /* TAIKO_ANIMATION_SCALE_H */
