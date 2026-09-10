# Scene transition timing

High refresh exposed a mismatch between Lumen's elapsed-time animation clock
and native controllers that still decremented delays once per rendered frame.
A 60-frame Results wipe wait consequently lasted only 250 ms at 240 Hz, while
the movie still needed its authored animation time. Advancing the controller
early can seek past the incoming animation or retire the outgoing scene before
the replacement covers it.

With `TAIKO_ANIMATION_TIMING=1`, `taiko_animation_frame_ticks()` now exposes the
same whole 60 Hz ticks used by `taiko_lumen_scale_frame_delta`. Each consumer
reads the same value; reading it does not consume a tick. Delays remain in
authored frames throughout their lifetime, including when render cadence
changes during loading. Boot fast-forward and `TAIKO_ANIMATION_TIMING=0`
retain one tick per guest frame.

The preserved insertions in `tools/recomp_hand_edits.json` cover:

| Guest function | Timing operation |
| --- | --- |
| `0005D864` | EnsoProcess fixed delayed-callback slots at `+14C`, stride `14` |
| `0005DA38` | EnsoProcess queued delayed callbacks, vector at `+18C/+190` |
| `001E2114` | GameEnso wait at `+170`, before controller readiness checks |
| `001E2E00` | Results countdown predicate at `+18`, including the wipe wait |
| `001E42A0`, `001E4CDC` | Results countdowns preceding the shared transition request |
| `001E690C` | Results 90-frame wait at `+88` |
| `00238AF0` | Player Entry idle voice repeat countdown at subcontroller `+38` |

The host browser launch path in `taiko_pc_mode.cpp` uses these ticks for its
120-frame transition wait too. No whole scene update is skipped: input,
readiness checks, callback dispatch, and rendering still run at the guest's
configured cadence. Expired callbacks keep their original dispatch behavior.
Chart time and the audio device clock are unchanged.

Player Entry's voice subcontroller is embedded at controller `+FEC`.
`001EF624` updates it through `00238AF0` every guest frame. When its `+38`
countdown goes negative, `00238944` selects an entry prompt, calls `00237EA8`
(which plays the cue through `001D53C4`), and reloads the configured period
from `+3C`. Scaling only the decrement prevents high FPS from restarting the
voice early; enable/suppression flags, prompt selection and input remain stock.
The timing test also exercises a repeating countdown at 60/120/144/240 Hz.

The intermission rainbow asset contains authored timeline labels (`in`, `out`,
`in_extra`, `out_extra`) and short Stop/goto actions. Its timeline already uses
the shared Lumen player update; it does not need a separate FPS multiplier.

The fractional accumulator now uses double precision to avoid rounding a
remainder just below one plus the four-tick catch-up limit up to five ticks.
The existing four-tick cap and loading-gap policy are otherwise retained.

Validation: `taiko_animation_timing_tests` exercises the real flip/tick/Lumen
helpers at 60, 120, 144, and 240 Hz, changing cadence, boot, disabled timing,
and long gaps. `taiko_pc_mode_tests` verifies that 480 render updates at
240 Hz do not prematurely finish the 120-tick launch wait. The user also
confirmed the original transition timing issues fixed in live play.

## Host browser handoff

The browser uses a separate 320 ms, monotonic-time smoothstep animation. On
launch its left/right panels tuck outward by up to 96 logical pixels and its
footer drops by 48 pixels while all three fade over the live native rainbow.
After Results, the browser panels ease into place over black. Neither direction
changes guest scene ownership, the 120-tick preload wait, or audio timing.

The overlay snapshots three opaque panels once per handoff, then translates
and fades those cached textures. This avoids independently fading overlapping
text/card layers and avoids per-frame texture uploads. Normal browser rendering
returns to native-resolution text after the animation. CPU/KMS fallback uses
the same panel layout and alpha. The GPU visitor marks the outgoing animation
as an overlay so the renderer continues drawing the underlying guest.

The clock starts with the first rendered handoff frame, not before synchronous
loading. Fresh browser input/publication cancels a handoff; explicit hide still
hides immediately. Cache tests cover panel placement, opacity, texture reuse,
GPU/CPU mode selection, completion, and unrelated login screens. For midpoint
GPU previews, append `enter` or `leave` to:

```sh
build-linux/taiko_browser_gpu_preview 1280 720 /tmp/taiko-handoff.bmp enter
```

Both midpoint previews rendered successfully on desktop Vulkan with zero
renderer errors and were visually inspected. The new handoff still needs a
live song/Results visual check.
