# Portable RSX with SDL3

The RSX command decoder now records owning, backend-neutral render batches.
The native null backend consumes those batches for headless tests, while every
graphical build consumes them through SDL3's SDL_GPU API. SDL chooses Vulkan on
Linux and can choose D3D12 or Vulkan on Windows; no project-owned D3D12 renderer
is compiled anymore.

## Backend configuration

The cache variables are independent:

- `TAIKO_RSX_BACKEND=null|sdl_gpu`
- `TAIKO_INPUT_BACKEND=null|sdl3`
- `TAIKO_AUDIO_BACKEND=null|sdl3|wasapi`

SDL3 audio is the default on Linux and Windows. WASAPI remains available on
Windows only as a temporary A/B oracle until live validation is complete.
`TAIKO_HEADLESS=ON` is a compatibility shortcut which forces all three to
`null`; CMake rejects an explicitly selected non-null backend in the same
configuration. `PS3RECOMP_NULL_RSX=1` remains a runtime renderer override.

## Native SDL build

Install the pinned developer dependencies once:

```sh
scripts/setup_sdl_gpu_linux.sh
```

This creates `third_party/sdl-gpu-linux` with SDL 3.4.10, the pinned shadercross and
SPIRV-Cross revisions, and Microsoft's checksum-verified DXC v1.8.2502 binary.
Native streamed-song decoding also uses the system FFmpeg development packages:
`pkg-config`, `libavformat`, `libavcodec`, `libswresample`, and `libavutil`
(the corresponding Debian/Ubuntu packages end in `-dev`). CMake checks these
modules when `TAIKO_INPROCESS_ATRAC=ON`, which is now the Linux default.
Then configure and build:

```sh
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DTAIKO_RSX_BACKEND=sdl_gpu \
  -DTAIKO_INPUT_BACKEND=sdl3 \
  -DTAIKO_AUDIO_BACKEND=sdl3 \
  -DTAIKO_EMBED_PPU_IMAGE=OFF
cmake --build build-linux
./run-taiko-linux.sh
```

The run script supplies the DXC library path and title defaults.

## Windows cross-build

Install the corresponding pinned target dependencies once:

```sh
scripts/setup_sdl_gpu_mingw.sh
```

Then configure with `mingw-w64.cmake` as usual. SDL_GPU and SDL3 input are the
defaults, so the explicit backend arguments are optional:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=mingw-w64.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
./run-taiko.sh
```

The build copies the pinned `dxcompiler.dll` and `dxil.dll` beside each SDL_GPU
executable. Native Windows lets SDL choose its GPU driver automatically. The
Wine launcher selects Vulkan directly. Set `TAIKO_GPU_DRIVER=vulkan` or
`TAIKO_GPU_DRIVER=direct3d12` to make driver selection explicit when debugging.

## Audio sink validation

SDL is initialized once on the main thread for the union of video, gamepad,
and audio subsystems. Shutdown stops cellAudio and releases input/render
resources before the process-wide SDL shutdown. The SDL3 sink accepts only
complete 256-frame, 48 kHz stereo-float blocks. It prebuffers six blocks and
then holds the device stream's input queue near that level.

Guest notifications are released on an absolute 5.333 ms block-period deadline,
not on queue depth alone. A host device typically pulls a whole period at once
(four blocks on the Pi's ALSA path), and pure queue backpressure hands the guest
those four notifications back to back. cellAudio's contract identifies the
target block through a single mutable `readIndexAddr` that the guest re-reads
per notification, so a burst makes it copy its mix into the same block several
times and silently destroy the rest -- audible as a song jumping forward while
the rest of the game keeps real time. The pacing loop pulls its period by
+/-12.5% to walk the queue back after a starvation dip without ever bursting;
`audio_sink_wait_for_block` remains the hard ceiling and the long-term clock.

`TAIKO_AUDIO_LOOKAHEAD_BLOCKS` (default 2) publishes `readIndexAddr` that many
blocks ahead of the block being played, so the producer has that many periods to
complete its event -> SPU mix -> copy round trip instead of the single period the
PS3 contract allows. `1` restores stock behaviour. Each block is 5.33 ms of
added output latency; `TAIKO_AUDIO_OFFSET_MS` compensates the song against it.

Consumed blocks are zeroed and tagged, so a block the guest fails to refill in
time plays as silence rather than repeating the previous ring revolution, and
the surviving tags are a non-blocking count of missed producer deadlines
(`UNFILLED`). Do not turn that into a handshake: blocking the device-paced mix
thread on the guest producer converts a late block into a real device underrun.

A failed or stalled device enters an explicit absolute-deadline null clock
instead of running the guest mixer at producer speed.

Use these backend-neutral diagnostics for paired WASAPI/SDL3 captures:

```sh
TAIKO_AUDIO_SINK_TRACE=1 \
TAIKO_AUDIO_RING_TRACE=1 \
TAIKO_AUDIO_DUMP=build/audio-sdl3.wav \
TAIKO_AUDIO_DUMP_SECONDS=30 \
RSX_PROFILE=1 ./run-taiko.sh
```

`[cellAudio-sink]` reports block rate, queued frames/blocks, complete submission
failures, dropped guest notifications, SDL device-facing starvation, and
cumulative RACE/STALE/UNFILLED counts once per second. The WAV contains the
exact complete blocks accepted by the selected sink, so it can be compared
directly across `sdl3` and `wasapi`. Validate attract BGM, selection previews, a
full song, Don-chan voice/VAG effects, transitions, shutdown during playback,
and the established roughly 60 FPS graphics workload before deleting WASAPI.
The native Linux path has also been exercised against a real PipeWire playback
device at 187.5 blocks/s with non-silent ATRAC and lifted-SPU output.

## Capture and replay

F10 arms a portable capture at the next completed visible-frame boundary.
For unattended capture use:

```sh
RSX_BATCH_CAPTURE=/tmp/scene.rsxb RSX_BATCH_CAPTURE_FRAMES=1 \
  ./run-taiko-linux.sh
```

The SDL capture path waits for queued rendering, downloads persistent display,
offscreen, and depth surfaces on the SDL main thread, and then records complete
immutable batches. Captures contain game-derived shader, geometry, and texture
data and are ignored by Git.

Replay validates the entire file before creating GPU resources and writes one
fixed 1280x720 BMP per batch:

```sh
build-linux/rsx_replay --backend=sdl_gpu \
  --input=/tmp/scene.rsxb --output-dir=/tmp/scene-frames
```

`--inspect-only` prints the ordered clear/draw chain without opening a GPU
window. For pass-lifetime debugging, `--stop-after-op=N` truncates replay after
one operation and `SDL_GPU_VIEW_SURFACE=0xOFFSET` presents and saves that RGBA8
offscreen surface instead of the display target. `--dump-textures=DIR` writes
captured RGBA8 payloads as dimension-labelled `.rgba` files and other formats
as `.bin` for asset inspection.

Useful renderer diagnostics are `SDL_GPU_DEBUG=1`,
`SDL_GPU_DUMP_SHADERS=1`, and `SDL_GPU_DUMP_TEXTURES=1`. A shader, pipeline, or
resource failure increments the replay error count; shader/pipeline failures
also replace the affected target with visible magenta.

Fragment texture registers are not assumed to be dense in guest programs. The
shader compiler packs the actually sampled RSX units into SDL_GPU's required
dense resource slots and stores the inverse mapping with the compiled shader;
draw binding therefore preserves cases such as a shader sampling only unit 1.
