# Controlled chart/audio sync test

`tools/prepare_sync_test.py` adapts peppy's Keyboard Latency Test using the
existing Zucchini connector converter. Run it with that connector's Python
environment (which provides `tja2fumen` and `rosu_pp_py`):

```sh
PYTHONDONTWRITEBYTECODE=1 /path/to/Zucchini-connector/.venv/bin/python \
  tools/prepare_sync_test.py '/path/to/57424 peppy - Keyboard Latency Test.osz' \
  --connector /path/to/Zucchini-connector
TAIKO_AUDIO_LOOKAHEAD_BLOCKS=2 bash scripts/run-sync-test.sh
```

The input is specifically the 29-circle standard-mode latency test. The script
checks its objects before changing the copied chart to taiko mode. It extends
the original 994 ms first-note time at one-second intervals to 180 notes, and
loops an exact 30-second decoded PCM segment six times. The converter adds the
same 506 ms lead-in to chart and audio. Green receives 91 measures, below its
300-measure limit, big-endian fumen, and ATRAC3plus NUB/NSH audio.

Generated files live under the ignored `game/sync-test/`. A separate VFS root
redirects the five solo charts and NUB/NSH of `mikukg` (Kagerou Daze). Other
assets are symlinked from the existing dump. The original song files and
connector source are untouched. The test launcher uses the `usrdir` VFS layout
so direct `/data/` and full `/dev_hdd0/.../USRDIR/data/` paths resolve identically.
The host browser labels the slot **SYNC TEST - 3 minutes (180 notes)**.
Normal launch scripts continue using the normal VFS.

Each launch creates a timestamped `runs/` directory containing a manifest,
frame-pacing/resource/audio logs, and up to ten minutes of submitted PCM.
Record the perceived starting alignment, any stutter time, and whether the
offset persists. Repeat with identical audio compensation and lookahead.
F3/F4 changes are logged and intentionally slew the active audio position;
exclude those intervals when measuring spontaneous drift.

The PCM dump starts at the first non-silent submission after gameplay is
armed. Its file position is therefore **not** an absolute chart-clock anchor.
It can establish audio continuity and rate; measuring absolute launch alignment
also needs the chart position or a synchronized visual/input observation.
The sink trace does not enable the separate UNFILLED diagnostic (`UNFILLED=off`).
Do not interpret that field as a measured zero.

## Audio-driven automatic hits

Regenerate with `--auto-tones`, then launch with:

```sh
TAIKO_SYNC_AUTO_HIT=1 TAIKO_AUDIO_LOOKAHEAD_BLOCKS=2 bash scripts/run-sync-test.sh
```

This version replaces the original click track with 180 isolated Gaussian
4 kHz pulses, peaked at the chart's note timestamps before encoding. Disable
in-game drum sound effects. SDL's final device-mix callback detects the tone's
energy peak and schedules a P1 centre pulse through USIO, independently of the
chart clock. Only the exact configured test NUB's gameplay decoder arms it;
deleting that decoder disarms it. Other songs and menus do not arm the detector.

`[sync-hit]` reports estimated output peak time, guest input consumption time,
late delivery in milliseconds, current compensation, and pending-hit collisions.
F3/F4 can adjust compensation while the game judges the automatically played notes.
Detection uses 1 ms windows and a 750 ms refractory interval. The encoded test
produced exactly 180 detections offline. The first decoded peak is at 1.5505 s
against the chart's 1.5000 s note: the test deliberately retains encoder delay
so calibration includes the actual audio pipeline.

Output time is estimated as callback time plus one device period plus the
peak's frame position. This is a repeatable software reference, not a measurement
of PipeWire/hardware/speaker latency. Device callback jitter and USIO poll delay
remain visible. Do not interpret `late_ms` as the game's judgment error; the
game's on-screen judgments supply that comparison. Automatic detection currently
requires 48 kHz device output and reports unsupported rates without injecting hits.

## Experimental stall recovery

`TAIKO_AUDIO_CLOCK_RECOVERY=1` enables decoder-side recovery for non-looping
gameplay songs. It learns a fixed source/time baseline during seconds 3–5 of
decoding, after the observed startup prefill pause. It excludes the applied
user offset from that baseline. Later lag beyond two ATRAC blocks plus 20 ms
triggers a forward seek, retaining one block of slack and crossfading for 5 ms.
`[audio-clock]` reports anchoring and corrections. Seeks reset the baseline;
looping tracks and previews are excluded.

This is opt-in pending live validation. It does not establish an authoritative
chart-start anchor, cannot correct startup drift learned during its calibration
window, and cannot bypass PCM already buffered downstream of the decoder.
Allow five seconds of clean gameplay before stressing this experiment.
It aims to test recovery from persistent mid-song drift without introducing
a blocking producer handshake or altering the saved audio offset.

The September 9 load test also exposed a separate guest timer loss. In
`0025B6A8`, elapsed time is read through `0035C9D8`, then the stopwatch is reset
through `0035CA48`. Preemption between those calls discards that interval.
Run `20260909-145955` showed approximately 104 ms of persistent guest-clock
loss while recovered output peaks returned to approximately 13 ms of their
initial cadence. The timer also interpolates an external sound reference;
it is not simply an unmodified wall clock.

`TAIKO_GUEST_CLOCK_TRACE=1` records that timer before its update.
The default guest-clock correction resets this specific stopwatch to
the preceding sample timestamp, preserving the intervening time for the next
update. Thread-local pairing checks both guest return addresses and the timer
object. `[guest-clock-preserve]` reports preserved intervals of at least 1 ms.
Set `TAIKO_GUEST_CLOCK_ATOMIC=0` to disable it for comparison.

In run `20260909-150535`, the user reported that repeated load and low FPS
no longer caused persistent desynchronization. Through note 174, output peak
cadence differed from its initial baseline by approximately 13 ms. The trace
recorded seven preserved intervals of 1.07–2.39 ms and a maximum timer sampling
gap of 35.865 ms. Decoder recovery was enabled but logged no forward
corrections in this sample, so this run does not independently validate its
behavior under a decoder stall. Decoder recovery remains opt-in. The narrow
guest timer correction is now enabled by default; the existing audio tests
do not directly exercise the guest caller/stack guards.
