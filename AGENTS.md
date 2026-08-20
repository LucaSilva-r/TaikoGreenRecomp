# TaikoRecomp — agent guide

Static recompilation of **Taiko no Tatsujin** (Bandai Namco, System 357 arcade
board = PS3 hardware, title `SCEEXE001`, S111 "Green") to native Linux and
Windows executables. SDL3 provides the portable graphical host. Uses the
[ps3recomp](https://github.com/sp00nznet/ps3recomp) framework (git submodule at
`ps3recomp/`). No game binaries/data in the repo — the dump lives in `game/`
(untracked).

## How it works

- `game/EBOOT.elf` — the game's PPU ELF, straight out of `unfself.py`. It is
  never patched: the Green dongle/VU bypass is applied to the lifted code by
  `tools/apply_recomp_patches.py` instead.
- `src/recomp/ppu_recomp_*.cpp` — ~8 chunks of lifted PPU code (millions of
  lines, generated; each guest function is `func_00XXXXXX(ppu_context*)`).
  Grep here to inspect guest code by address.
- `src/gen/` — generated HLE stubs + NID table.
- `ps3recomp/runtime/` — PPU loader/VM (flat 4 GB guest space at `vm_base`,
  demand-committed via VEH on Windows), HLE dispatch, lv2 syscalls, cellFs VFS.
- `ps3recomp/libs/video/` — RSX HLE: `cellGcmSys.c` (FIFO drain, flips,
  offsets), `rsx_commands.c` (NV4097 method decode → `rsx_state`), the portable
  recorder/batch model, and `rsx_sdl_gpu_backend.c` (SDL_GPU execution,
  offscreen RTs, shader translation, pipeline cache, and presentation).
- Title-specific shims in `src/`: `taiko_usio.cpp` (virtual PS3A-USJ arcade I/O
  board + backup SRAM + network-state spoof), `taiko_sail.cpp` (cellSail
  lifecycle so the movie wrapper doesn't hang), `taiko_net.c` (DNS loopback),
  `taiko_init.c`.
- `ghidra_out/` — `functions.json`, `symbols.json`, `strings.json` from Ghidra:
  the map for symbolizing guest addresses (OPD pointers, string refs).

Frame flow: guest writes the GCM FIFO → host vblank-ticker thread drains it at
~4 ms (`cellGcm_rsx_process_fifo`) → the recorder snapshots backend-neutral,
owning render batches → the SDL main thread executes them with SDL_GPU and
presents. Display clears and flips delimit complete batches without exposing a
partially recorded frame.

## Build

Two targets. `build/` is the MinGW/Windows build run under Wine; `build-linux/`
is the native Linux build. Both read their dependencies from `third_party/`, so
either build directory can be deleted and recreated freely.

```sh
scripts/setup_sdl_gpu_mingw.sh # once: pinned SDL3 + shadercross + DXC target bundle
scripts/setup_sdl_gpu_linux.sh # once: the same bundle for the native build
scripts/build_ffmpeg_mingw.sh # once: pinned minimal static ATRAC3plus decoder

# Windows (via mingw-w64 + Wine)
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build          # incremental; full link of the lifted code is slow

# Native Linux
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DTAIKO_RSX_BACKEND=sdl_gpu -DTAIKO_INPUT_BACKEND=sdl3 \
  -DTAIKO_AUDIO_BACKEND=sdl3 -DTAIKO_INPROCESS_ATRAC=ON \
  -DTAIKO_EMBED_PPU_IMAGE=OFF
cmake --build build-linux
```

**Parallelism is capped on purpose** (`TAIKO_COMPILE_JOBS`, default 4, via a
ninja job pool). Each lifted chunk is a 600k–800k line TU needing several GB at
`-O2`; ninja's default of one job per core meant 16 concurrent multi-GB compiles,
which exhausts RAM and wedges the machine rather than merely being slow.

Budget **~3 GB per job** and set it from available RAM, not core count:

```sh
cmake -S . -B build -DTAIKO_COMPILE_JOBS=8 ...   # ~24 GB peak, needs 32 GB+
```

The default stays at 4 so a 16 GB machine builds unattended. On a 46 GB box 8
is comfortable and roughly halves a full rebuild; going to `nproc` is what
caused the original wedging. It is a cache variable — pass it at configure
time, or edit the cache and re-run cmake.

Changing the chunk count (a re-lift) needs a **re-configure**, not just a build —
`file(GLOB)` is evaluated at configure time.

Stale objects used to be a second trap here: ninja compares mtimes, and a
freshly lifted chunk can look older than the object built from the previous
lift, so the link fails with undefined references to `func_*` (a re-lift moves
functions between chunks). `apply_recomp_patches.py` now stamps every chunk to
the current time, so this resolves itself. If you ever lift *without* running
it, `rm -f build*/CMakeFiles/taiko_boot.dir/src/recomp/*.obj` is the manual
fix.

## Run

```sh
./run-taiko.sh               # logs to build/taiko.log (TAIKO_CONSOLE_LOG=1 for stdout)
```

The script sets everything needed. Notable pieces:

- `TAIKO_GPU_DRIVER=vulkan` makes the Windows SDL_GPU build use Wine's Vulkan
  path directly. Native Windows leaves the variable unset so SDL can select
  `direct3d12` or `vulkan`. The old vkd3d-proton override is obsolete.
- `PS3_VFS_ROOT` — host dir the PS3 mount points map into (`game/vfs`).
  **All of the game's mount points must exist there**, not just `data`. The
  title also uses `/cache`, `/hash`, `/install`, `/logs` and `/updates`, all of
  which live under the RPCS3 dump's `USRDIR/`. Symlink each one:

  ```sh
  R="/path/to/rpcs3/.../dev_hdd0/game/SCEEX001 GREEN/USRDIR"
  cd game/vfs && for d in cache hash install logs updates; do ln -s "$R/$d" "$d"; done
  ```

  A missing `/cache` is not a soft failure: the loader reads
  `/cache/S11100-1/texturecache_*.bin` and, on a miss, *writes* the cache back.
  With no parent directory both fail and the asset load stops dead while the
  render loop keeps animating -- it looks exactly like a hang on the LOADING
  screen. Adding the mounts took one boot from 895 file opens to 1300+.
- `TAIKO_DNS_LOOPBACK=1` — arcade network services resolve to 127.0.0.1.
- **Boot fast-forward** (`boot_fast=1` in `taiko_online.cfg`, on by default).
  The whole boot — arcade system checks, the chassis service sequence and the
  asset load — is paced by the guest's per-frame state machine, not by the
  network or by disk: measured round trips to the server are ~250 ms while the
  gaps between service calls are 4–9 s of an idle guest. The frame driver
  therefore ticks vblank at 240 Hz until the title starts its attract audio
  (`ps3_frame_boot_fast_finish`, called from the first ATRAC decode) and then
  returns to 60 Hz; a 180 s deadline is the backstop if that never happens.
  Measured: 26 s of chassis calls → 6 s, asset loading starting at 6 s instead
  of 22 s. `TAIKO_BOOT_VBLANK_HZ` sets the boot rate, `TAIKO_VBLANK_HZ` the play
  rate. Do not raise the play rate — everything frame-paced, including chart and
  audio timing, scales with it.
- Boot takes ~1–2 min: security/test screen → Bandai Namco logo → credits →
  attract. The fumen `composition.xml` scan (~850 files) is the long pause.

## Debugging tools

Everything here exists in the **shipping SDL_GPU backend**
(`ps3recomp/libs/video/rsx_sdl_gpu_backend.c`) and the portable recorder. The
old D3D12 backend and its switches (`F9` capture, `TEXDROP`, `RTT_DUMP`,
`CELLMARK_DUMP`, `TEX_RAW_DUMP`, `CLEAR_DBG`, `FP_NOWHITE`, `D3D12_IQ`,
`VP_LEQUAL_LESS`) are **gone** -- do not document or reach for them.

- `RSX_BATCH_CAPTURE=<file>` + `RSX_BATCH_CAPTURE_FRAMES=N` — record N frames
  of backend-neutral render batches to a `.rsxb`. This replaces the old F9
  capture and is the main tool: it captures what the recorder saw, so a bad
  frame can be replayed offline, repeatedly, without the game running.
- `build-linux/rsx_replay --backend=sdl_gpu <file.rsxb>` — replay a capture.
  Deterministic, and the fastest way to tell a recorder bug from a renderer
  bug: if the capture replays correctly in a fresh process, the defect is in
  what was recorded, not in how it was drawn.
- `RSX_RESOURCE_TRACE=1` — per-frame resource accounting; prints
  `[SDL_GPU-SLOWPREP]` with new shader/pipeline/texture/sampler counts whenever
  preparation exceeds 5 ms. This is what identified the Go-Go slowdown.
- `RSX_FPS_LOG=1`, `RSX_FRAME_PACING_TRACE=1` — frame rate and pacing.
- `RSX_TEXTURE_CACHE_LIMIT=16..1024` — shrink the sampled-texture cache to
  force continuous eviction; how the long-session white-rectangle bug was
  reproduced on demand.
- `SDL_GPU_DEBUG=1` — SDL_GPU validation layers.
- `SDL_GPU_DUMP_SHADERS=1`, `SDL_GPU_DUMP_TEXTURES=1` — dump translated
  shaders and uploaded textures. `python3 tools/bc_decode.py <file>` turns a
  raw BC/ARGB texture into a BMP.
- `SDL_GPU_VIEW_SURFACE=<hexoff>` — show one offscreen render target
  fullscreen (the replacement for `RTT_VIEWRT`).
- `TAIKO_PRESENT_MODE`, `TAIKO_FRAMES_IN_FLIGHT`, `TAIKO_GPU_DRIVER` —
  presentation and driver selection.
- `PS3RECOMP_NULL_RSX=1` / `PS3RECOMP_NULL_AUDIO=1` — headless backends, used
  by `scripts/test-linux-headless.sh`.
- Guest-side: `[WAIT]`, `[fs]`, `[taiko_usio]`, `[taiko_netstate]` lines in
  the log; `sys_memory` allocs log caller `lr`.
- **Free ledger** (`ppu_note_free` / `[freelog]` in `ppu_loader.cpp`) — the
  guest allocator poisons freed blocks with `0xDD`, so a use-after-free is
  visible (`0xDDDDDDxx` in a pointer/TOC/vtable slot) but anonymous. The lifted
  guest `free()` (`func_005A93CC`) feeds an 8192-entry ring of
  `{ptr, lr, tid}`; the garbage-vcall and `[TOCBAD]` guards look the offending
  pointer up and print which call site freed the block, how far into it the
  pointer lands, and how many frees ago. Always on, no env var. Run *without*
  `FLOW_NOOPVCALL` when hunting: that flag turns bad calls into `r3=0`, which
  the game then stores as an object pointer and dispatches through, burying the
  original fault under a cascade of null-vtable calls.
- **`tools/hangstack.py <pid>`** — for a hang, attach gdb to the live Wine
  process and print the guest call chain of whichever thread is still running
  (`pgrep -f 'taiko_bo[o]t'` for the pid). gdb can't unwind PE frames, so it
  scans the host stack for return addresses inside `taiko_boot.exe`; each
  lifted guest function is a host function `func_XXXXXXXX`, so the chain is
  the guest chain. Prints `vm_base` too, so guest memory reads straight from
  gdb: `gdb -p PID -batch -ex 'x/8wx <vm_base>+<guest_addr>'` (big-endian
  words). Threads parked in a futex are blocked waiters, not the culprit.
  The `HOTREAD*` spin reporters only fire on *one* repeated address — a loop
  alternating between two addresses is invisible to them; `YDKJ_HOTMAP=1`
  (hottest read per 8M-read window + `[SPINBT]` host rvas) does catch those.

## Current state / known issues (as of 2026-08-16)

- Game boots to credits/attract; gradient backgrounds and direct-drawn UI
  render. Layer flicker (text/gradient/icon alternating per frame) was fixed
  by gating flip-presents while a display-clear-owned batch accumulates.
- **Open graphics bug**: credits text is invisible. It is drawn directly to
  display sampling the DXT23 font atlas at RSX-local offset `0xCC0300`;
  suspicion: the game fills this local-memory texture via a path the backend
  doesn't emulate (CPU write into mapped local memory, or an RSX 2D-engine
  transfer — `NV0039/NV309E/NV3089` are stubbed in `cellGcmSys.c`). Capture the
  frame with `RSX_BATCH_CAPTURE` and check whether the atlas has content at
  draw time; an unsupported texture format was exactly what hid the transition
  rainbow, so verify the binding survives before theorising.
- Transition wipes render the rainbow again (fixed 2026-08-13, `0x9E` /
  D8R8G8B8 was being dropped at bind time).
- **Player Entry graphics are repaired.** The texture deswizzle, SRV collision,
  and whole-Lumen-group ordering changes are general renderer fixes. The final
  title issue was different: offline Green left nested sprite 817 stopped and
  hidden on frame 0. `src/taiko_lumen.cpp` contains a deliberately narrow,
  once-per-process release guarded by vtable, character ID, frame count, frame,
  stop state, and the real offline callback state/flag. It then lets the
  original timeline animate to frame 20. `TAIKO_OFFLINE_COMPLETE=0` disables
  both offline callback completion and this hook. Do not generalize it to other
  stopped clips. See `docs/player_entry_graphics_hotfix.md`.
- **Native SDL_GPU Player Entry renders Don-chan** (2026-08-16). Indexed
  triangle-strip capture now honors the RSX primitive-restart marker and resets
  strip parity at each segment. The remaining disappearance was a recorder
  state bug: `CLEAR_SURFACE` pushes a new attachment through
  `set_render_target` without a draw-time `sync_state`, but the recorder only
  forwarded that callback and retained the previous pass. Clears were therefore
  attached one target late and erased the finished 600x600 character RT.
  `rec_set_render_target` now snapshots the pushed state. An F10 replay proved
  the mesh was valid before the misplaced clear and the repaired attachment
  chain restores it in the final composition. Don-chan's initially blank face
  was a separate portable-binding bug: its fragment program samples only RSX
  texture unit 1. ShaderCross reflected one sampler, which the backend assumed
  meant guest unit 0, leaving unit 1 unbound. Fragment resources are now packed
  into dense SDL slots while each shader retains a dense-slot-to-RSX-unit map;
  the captured 128x128 eyes/mouth atlas then renders correctly.
- The Bandai Namco logo is **not** missing: it fades in, and a faint capture was
  simply taken mid-animation. The mostly black attract screen is a video; the
  movie/video path is not rendered yet. Neither should be diagnosed as the
  loading hang. Inserting coins from attract reaches the rendered player-entry
  screen.
- UI is composited through offscreen RT chains (448×256 tiles →
  `0x1AE1000`/`0x2069000`); `off_rt_*` in the backend keeps those persistent
  across frames. Depth is one shared buffer, **cleared on every RT switch**
  in the vp pass — a known hazard for display-depth continuity.
- Loading is now stable in the current sample: five consecutive scoped-lock
  boots reached attract. The three pre-checkpoint logs
  (`taiko-typed-registry-01/02/03.log`) recorded 1326, 1326, and 1325 file
  opens; the two clean-tree post-checkpoint logs (`taiko-postcommit-01/02.log`)
  recorded 1325 and 1326. All five had zero `xml-fatal`, allocator assertion,
  invalid-free, `TOCBAD`, unresolved-indirect, or abort markers. Continue
  watching for rare races; five boots are strong evidence, not a proof.
- **Frame performance is repaired** (updated 2026-08-16). A non-default
  Don-chan costume exposed a second, much larger recorder bottleneck: the
  portable recorder cleared its texture snapshot cache after every submitted
  batch. The exact 177-draw reproduction consequently deswizzled/copied 14.48
  MiB of unchanged textures every frame, spent 55–58 ms/frame in
  `snapshot_texture`, saturated the FIFO thread at ~935 ms CPU/s, and rendered
  at 15–16 FPS even though SDL_GPU execution itself took only ~8 ms/frame.
  Texture payloads now persist across batches behind a raw guest-memory
  fingerprint; modified textures refresh, recorded render-target surfaces
  bypass the cache, skinned vertex arenas remain strictly per-frame, and an
  LRU/256 MiB bound limits retained payloads. The same 177-draw class now holds
  59.8–60.1 FPS: unchanged payload copies are 0 MiB/frame, fingerprinting costs
  ~2.1 ms/frame, and FIFO load is ~240–290 ms CPU/s. One-second dips during
  transitions can still reflect intervals where the guest submits no complete
  frame or synchronously decodes a new jingle; those are not sustained GPU
  slowdowns.
- **Rainbow/Go-Go gameplay slowdown is repaired** (2026-08-16). Filling the
  life meter activates animated lower-screen characters and an RSX fragment
  program whose inline constant payload changes every frame. The recorder and
  SDL shader cache previously hashed the complete program bytes, while the
  decompiler embedded those constants as HLSL literals. That made every frame
  a distinct shader/pipeline and spent about 78 ms/frame compiling it; the
  measured SDL preparation time rose from 0.03 ms to 71.78 ms while actual
  rendering remained only 2–3 ms. Fragment programs are now keyed by a
  structural hash that excludes inline payloads, SDL shaders read the exact
  values from a per-draw uniform buffer, and the pipeline cache canonicalizes
  old captures the same way. A three-frame F10 replay produced no new shader or
  pipeline after its cold frame, and live Go-Go gameplay held 59.9–60.1 FPS
  with roughly 0.05–0.12 ms preparation and no visible slowdown.
  `RSX_RESOURCE_TRACE=1` prints `[SDL_GPU-SLOWPREP]` breakdowns for preparation
  taking more than 5 ms, including new shader/pipeline/texture/sampler counts.
- **Long-session white rectangles are repaired** (2026-08-16; live validated).
  The SDL sampled-texture cache had a fixed 1024-entry table with no
  eviction. On the first miss after it filled, `get_sample_texture` returned
  the diagnostic white texture; later song titles and then large parts of
  gameplay consequently became opaque white rectangles. The live trace proved
  the boundary exactly: renderer errors stayed at zero, then began increasing
  by about 120/s as soon as the table saturated. Captured draws replayed
  correctly in a fresh process because its cache began empty. The table now
  replaces its least-recently-used entry, relying on SDL's deferred safe
  texture destruction rather than waiting for GPU idle. A forced 64-entry
  replay of the 419-operation reproduction exercised continuous eviction and
  was byte-identical to the normal 1024-entry render. For stress testing,
  `RSX_TEXTURE_CACHE_LIMIT=16..1024` lowers the active limit.
- **Gameplay rainbow masking is repaired** (2026-08-16; capture validated).
  The rainbow transition is a two-draw stencil sequence: an invisible black
  952x384 texture alpha-tests a clean arch into stencil with `ALWAYS`, ref 1,
  `ZPASS=REPLACE`; the rotated colour texture then tests `NOTEQUAL` against
  ref 0. The portable recorder already retained this state, but SDL_GPU never
  configured or referenced stencil, so it drew the colour texture's entire
  opaque magenta interior. SDL now maps the RSX compare and keep/zero/replace/
  increment/decrement/invert/wrap operations into its depth-stencil pipeline
  and sets the per-draw reference. Replaying the captured frame restores the
  clean rainbow arch while preserving the life meter and scene beneath it.
- **Gameplay chart/audio synchronization repaired** (2026-08-16; live
  validation pending). Correcting the call-driven `sys_time_get_system_time`
  stub to use the shared monotonic 79.8 MHz PPU timebase was necessary but did
  not change the gameplay rubber-banding. The actual bnusCore synchronization
  path at `lr=0x00290888` calls `cellAudioGetPortBlockTag`, then
  `cellAudioGetPortTimestamp`. Both NIDs were unregistered: generic HLE returned
  fake success without writing either 64-bit out-parameter, so bnusCore used
  stale stack data as its audio clock. They now implement the RPCS3/PS3 contract
  using a monotonically increasing consumed-block tag and its corresponding
  guest-system timestamp, including `CELL_AUDIO_ERROR_TAG_NOT_FOUND` for future
  tags. `cellGcmGetVBlankCount` was also changed from a query-driven increment
  to a pure read; only the 60 Hz frame driver advances it.
- ~~Thread 5 spins on SPU event queue 5.~~ **Fixed 2026-08-12** — that was the
  audio mixer's `'END '` wait never being satisfied. Audio now works; see
  "Audio mixer" below.
- SPU images in `game/spu/` are lifted to `src/spu_gen/` (gitignored). Only
  spu_0004 (the audio mixer) has been exercised in depth; cellSpurs event flags
  are still force-satisfied.

## Loading hangs/crashes: broken lwmutex HLE (fixed 2026-08-11)

Boots that died at a different place every time — stuck LOADING, random aborts,
`0xDDDDDDxx` pointers, `[TOCBAD]`, garbage vcalls — were **guest heap corruption
from a data race**, not many separate bugs.

`sys_lwmutex_lock` (`ppu_sysprx.cpp`) was a **no-op owner stamp** providing zero
mutual exclusion. `sys_ppu_thread_get_id` also returned 1 for every context, so
the guest CRT could mistake unrelated threads for recursive ownership. The
title takes lwmutexes at **888 call sites across 523 guest functions**, 35 of
them in the allocator/CRT range `0x5A0000-0x5B0000`, with 17 guest threads
running.

- **Oracle**: the guest's own dlmalloc reports the damage —
  `chunksize(p) == small_index2size(i) -- assertion failed` (`malloc_lv2.c`).
  Grep `assertion failed`. That is corrupted chunk metadata, self-reported.
- **Race vs lifter bug**: run the same binary twice. Corruption that dies at
  *different* points (measured: 52 vs 884 file opens, different abort chains) is
  a race — runtime/HLE. Same point every time is the lifter.
- **Result**: the allocator assert, random garbage vcalls, early XML abort,
  `tuning.bin` spin, `body_000000.nud` stall, late resource `TOCBAD`s, and the
  white-screen open-hash spin all disappeared in five consecutive boots.

The host mutex is now a **fair/FIFO recursive timed mutex**; this matters because
`std::recursive_timed_mutex` allowed barging and starved waiters. Guest thread ID
1 is reserved for the initial PPU context and worker IDs begin at 2.

Serialization remains scoped rather than applying every guest lock. Allocator/
CRT locks are discovered by callers in `0x5A0000-0x5B0000` and retain the timed
fallback. Short, proven loader critical sections use strict (no timeout)
serialization: async job lifetime, callback slots, shared asset dispatch, typed
asset registries, resource hash tables/pools, and resource-object lists/refcounts.
The strict families were selected from full `PPU_LWM_ALL=1` traces and generated
code inspection; they are not a blanket address-range heuristic.

The failure signatures that identified each missing family were:

- fumen XML race: huge bogus `sys_memory_allocate` followed by `xml-fatal`;
- typed registry race: permanent `strlen` chain under `func_007D5034`, hottest
  bytes `0x01405A30/31`, stuck after opening `tuning.bin`;
- asset dispatch race: hard pause immediately after `body_000000.nud`;
- resource lifetime race: `0xCD` fields, poisoned frees at `lr=00537544`, and
  late `TOCBAD`s during accessory/model loading;
- mutable-sentinel hash race: `func_0054A7B8` followed a null chain forever.

`PPU_LWM_ALL=1` serializes everything for diagnosis but can break rendering or
drop to ~4 FPS when a guest thread holds a lock while parked on an unimplemented
wait. `PPU_NOLWM=1` restores the old no-op; `PPU_LWM_WAIT_MS` sets the timed
allocator deadline (default 200); `PPU_LWM_TRACE=1` logs each dynamically
observed lock caller once for a compact boot trace.

Also fixed: `stwcx.`/`stdcx.` had no cross-thread reservation break (ABA on
lock-free free-lists). Real bug, but **not** the cause here — corruption
persisted with it fixed. `PPU_RESV_OFF=1` restores the old value-CAS.

## Drum input (fixed and live-validated 2026-08-15)

The old keyboard path called `GetAsyncKeyState` only when Green read USIO
register `0x1080`. A complete press/release between two reads disappeared, so
rapid play missed many hits. Gameplay input is now independent of the Wine
window-message pump: a highest-priority thread samples keyboard state at 1 kHz
and XInput at 250 Hz, publishes current levels, and atomically latches rising
edges until the next guest board read consumes them. A validation boot held
998–1000 sampler polls/s without disturbing 60 FPS. Green consumes the board
at about 60 Hz in this path, so a captured hit normally reaches its next USIO
report within one 16.7 ms frame. Live play testing described the inputs as
feeling great.

Each latched drum hit becomes an analog peak followed by a short linear decay,
which resembles the arcade piezo sensor and keeps it visible across multiple
guest polls.

- `TAIKO_HIT_VALUE` controls the peak, default `0x0FFF`.
- `TAIKO_HIT_HOLD` controls the total number of reported polls, default 3.
- `TAIKO_INPUT_TRACE=1` reports sampler/report rates, report interval range,
  latched edges, and sample-to-USIO-consumption age. It also enables the noisy
  per-hit payload logs; leave it unset for normal play.
- The close button now terminates the whole executable from the harness rather
  than merely stopping RSX and leaving `ppu_run` alive. Escape is deliberately
  not an application-exit shortcut.

## Guest-visible races (fixed 2026-08-20)

Two defects of the kind that corrupt or hang at a different place every run,
so they read as many unrelated bugs.

**vblank/flip handlers ran guest code on a host thread.** `cellGcmTickVBlank` /
`TickFlip` called `g_ps3_guest_caller` directly from `frame_driver.cpp`'s
ticker, so guest handlers executed concurrently with the main guest thread.
The ticker now only marks a tick pending; `ppu_gcm_pump()` runs the handlers on
a guest thread at HLE-call boundaries, guarded by `ppu_in_guest_callback()`
because `ppu_guest_call` and `ppu_guest_call_ct` share one per-thread scratch
context. Pending ticks are collapsed, not replayed.

That depth counter is C++ `thread_local` **and must stay that way**. MinGW
silently ignores `__declspec(thread)` — the same defect that once let guest
threads steal each other's trampoline continuations.

Measured after the change: a live session delivered 75600/75600 ticks with a
maximum backlog of 1, and the frame stutters disappeared. `RSX_VBLANK_TRACE=1`
prints the drain/backlog counters. `TAIKO_GCM_TICKER_HANDLERS=1` restores the
old direct-from-ticker delivery for A/B — it races by design and exists for
measuring audio/chart timing, not for play.

**`sys_lwmutex` could not be released by a non-owner.** lv2 permits it and the
guest's own recursion counter decrements regardless of caller, so refusing left
the mutex held and hung every waiter. The subtlety: a genuine cross-thread
release is indistinguishable from an unlock issued by a thread whose *timed
acquire expired* — `try_lock_for` failing means the caller proceeds without the
lock and the guest still calls unlock. Expired acquires are now counted per
thread and balanced without touching the owner.

`FairRecursiveTimedMutex` lives in `runtime/ppu/ppu_fair_mutex.h` so it can be
tested; `tests/fair_mutex_tests.cpp` covers recursive ownership, FIFO fairness,
cross-thread release and the expired-acquire fallback, and fails against the
previous implementation on the cross-thread case.

## Upstream ps3recomp: what we already have (surveyed 2026-08-20)

`ps3recomp` is vendored at `82a1f96`; upstream master was 606 commits ahead.
**That number badly overstates the gap** — most of the threading work is already
here, some fixed independently. Verified present: sub-millisecond QPC timeouts
(`lv2_usec_deadline`), unique guest thread IDs, cross-thread `stwcx/stdcx`
reservations, honoured lwmutex timeouts, per-thread TLS blocks.

Still missing: `sys_net` `select`/`poll` must block for the guest timeout (see
`docs/online_base.md`). The rest of upstream's diff is largely LBP/WWS SPU work
that does not apply to this title.

Do **not** merge `runtime/ppu/ppu_sysprx.cpp` wholesale: upstream's is ~900
lines smaller and would replace the fair/FIFO mutex with a barging one,
reintroducing the waiter starvation documented above. Upstream's lifter branch
is also in master, and that is what previously rendered a black screen — treat
a lifter bump as its own project with a full re-lift and revalidation.

## Online

Full details in **`docs/online_base.md`**. Summary: every arcade service is
sent to one configured host and port over TLS.

- Configure with `taiko_online.cfg` next to the executable, or the
  `TAIKO_ONLINE_HOST` / `_PORT` / `_VERIFY` / `_CACERT` environment overrides.
  **No host configured means offline**, exactly as before, and the cellHttp
  transport hooks are not even installed.
- TLS is mbedTLS 3.6.4, vendored by `scripts/build_mbedtls.sh` into
  `third_party/mbedtls-{linux,mingw}` (needed before configuring, like the
  FFmpeg and SDL bundles). `src/taiko_tls.c` wraps it; nothing else talks to
  mbedTLS.
- Two transports reach the server. cellHttp (MUCHA, game server) uses its own
  native sockets and asks the title layer through the hook pointers in
  `ps3recomp/libs/network/cellHttp.c`. The ALL.Net PowerOn POST is a raw
  sys_net socket, retargeted and wrapped in `net_connect`
  (`src/taiko_net.c`) -- the guest keeps writing plain HTTP into a TLS session.
- **SNI and `Host:` must name the configured server, not the service the guest
  asked for.** Measured against a live ALL.Net server: `sni=naominet.jp` gets a
  fatal alert (-0x7780), and `Host: naominet.jp` gets a 403 from the CDN in
  front of it while the same request with the server's own name returns
  `stat=1`. Path and body are what the server routes on, and those are left
  alone. The raw ALL.Net socket composes its own headers, so
  `net_rewrite_host_header` patches that one line on the way out.
- The title sets `SO_NBIO`, so its connect returns EINPROGRESS; the hook waits
  for writability itself (5 s cap) and completes the handshake before returning
  a connected socket. The mbedTLS BIO must answer `WANT_READ`/`WANT_WRITE` for
  a would-block, not -1 -- returning -1 aborts the handshake with a generic
  error, which is what happened first.
- All 19 cellHttp and 26 sys_net imported NIDs are bound. `src/taiko_net.c`
  maps the SDK names this title uses (`cellHttpResponseGetStatusCode`,
  `cellHttpRequestSetHeader`, ...) onto the runtime implementations; the names
  were recovered by hashing candidates with the PS3 NID algorithm
  (SHA-1(name + suffix), see `ps3recomp/include/ps3emu/nid.h`).
- `socketselect` and `socketpoll` were previously registered to a stub that
  returned 0 without waiting. Both now marshal the guest big-endian
  `fd_set`/`pollfd`/`timeval` and block for the real timeout;
  `build-linux/net_select_tests` covers that marshalling and, with
  `TAIKO_TLS_LIVE_HOST` set, performs a real handshake.
- **The title reads a response through `cellHttpResponseGetStatusCode`, not
  `cellHttpRecvResponse`** -- so that query blocks for the response head.
  Getting this wrong looks like every arcade service retrying forever.
- Four defects in the vendored `cellHttp`/`cellHttpUtil` only surfaced once a
  real server answered: `ParseUri` took host pointers (segfault) and read
  `path` from the `username` slot; client/transaction handles started at 0,
  which the title reads as "invalid"; `CloseConnection` was aliased onto
  `AbortTransaction`. All fixed, with the parsing covered by
  `net_select_tests`.
- Live-validated 2026-08-20 against a private ALL.Net server: PowerOn over TLS,
  chassis startupauth 200, `online_state=2 ready=1`, and the whole chassis
  service loop running.
- **Card login without a reader.** `src/taiko_card.c` is the virtual
  BanaPassport behind the USIO reader emulation; `src/taiko_pairing.c` is the
  six-digit pairing client. `TAIKO_CARD_CODE=<20 digits>` swipes a known code
  with no server; `TAIKO_PAIRING_TOKEN=<token>` polls the server for one and
  logs the code to enter in its web UI. The card's block 1 is encrypted with
  key material inside the game image, found by its `NBGIC0..7` tags, so a code
  no profile issued is rejected rather than silently wrong. The code is drawn
  on screen by `src/taiko_overlay.c` (FreeType, the game's own font, vendored
  by `scripts/build_freetype.sh`) on Zucchini's pill artwork, drawn through the
  SDL_GPU backend's optional `g_rsx_overlay_frame` hook. That draw is an
  alpha-blended quad, not `SDL_BlitGPUTexture` -- a blit cannot blend, so the
  pill's transparent ends would punch holes in the frame.
  `TAIKO_OVERLAY_FONT` points at the font; it is not in the repo.
- `TAIKO_NET_TRACE=1` logs the first 40 socket operations.

## Repository layout and history

This tree was re-founded as a clean, publishable repository. Its history begins
at a single commit; the previous working repository is kept locally as
`TaikoRecomp-archive` and holds the full development history plus the
experimental `src/recomp.newlift/` snapshot (see "Lifter" below).

### Regenerating the lifted snapshot

`src/recomp/` is a *generated snapshot plus hand fixes*, and the hand fixes are
not optional -- a raw lift is missing the guest-heap, small-object and XML
tokenizer mutexes, the invalid-free guard and free ledger, and the dongle/VU
security bypass. Reproduce it with:

```sh
python3 ps3recomp/tools/ppu_lifter.py game/EBOOT.elf \
    --functions game/functions.json --code-end 0xa1f890 \
    --names meta/names.json --hle-stubs meta/EBOOT.imports.json \
    --extra-targets meta/jt_seeds.json -o src/recomp -j $(nproc)
python3 tools/apply_recomp_patches.py
```

The edits live in `tools/recomp_hand_edits.json`: 32 hand-edit functions
(186 lines), 4 security functions (57 lines), 5 helper declarations, and the
chunk preamble that declares the three recursive mutexes. It stores only
hand-written lines plus the generated line each anchors to -- never a lifted
body, so nothing derived from the executable is tracked. `apply_recomp_patches.py`
is idempotent and fails closed; `--no-security` omits the bypass.

To recapture the edits after a lifter change, lift once into a scratch
directory with no patches applied and run
`tools/derive_recomp_hand_edits.py --baseline <scratch> --patched src/recomp`.
It ignores functions whose only differences are lifter codegen drift.

**The ELF is never patched.** `tools/patch_taiko_security.py` and
`game/EBOOT.recomp.elf` are gone; the Zucchini dongle/VU bypass is applied to
the lifted code at five sites in `func_009287F4`, `func_00926F8C`,
`func_00927748` and `func_00939454`.

`tools/fix_vmrghw_snapshot.py` is retained but is a **no-op against the current
lifter**, which emits `vmrghw` correctly (it reports 0 rewrites on fresh
output). It only matters if you lift with an older tools checkout.

Validated 2026-08-20 on a full re-lift: 36/36 patched functions byte-identical
to the previous known-good snapshot, three headless boots at 1330 file opens
with zero fault markers, and a live boot reaching attract with the security
screen reporting OK.

`ps3recomp/` is **vendored, not a submodule** — flattened at upstream `82a1f96`
plus local changes. There is no gitlink and no `.gitmodules`; edit it in place
and commit with everything else. CMake reaches it only through `PS3RECOMP_DIR`.

`game/`, `src/recomp/` and `src/spu_gen/` exist on disk, are consumed by the
build, and are ignored by git — regenerate them from your own dump. `meta/` and
`ghidra_out/{functions,symbols}.json` *are* tracked (address and symbol maps);
`ghidra_out/strings.json` is not, and no build step reads it. `third_party/` holds installed dependency prefixes so
that any `build*/` directory is disposable — deleting one never costs an SDL3
or FFmpeg rebuild. See `NOTICE.md`.

## Audio mixer (fixed 2026-08-12)

Audio works: the bnusCore SPU mixer renders, `cellAudio` receives non-silent
blocks, and attract BGM is audible. The path is
`bnusCoreUpdateThread` -> `func_00379748` -> **`func_0037958C`**, which sends
`'STAT'` to the raw SPU mixer thread and waits for `'END '`, then copies the
mixer's 0x2000-byte output into the cellAudio port.

The old diagnosis in this file ("the SPU images are never executed, so the reply
never arrives") was **wrong**. The images run fine. Three lifter/runtime defects
were stacked on top of each other, each hiding the next:

1. **`spu_xswd` corrupted every 64-bit DMA effective address** (the silence).
   XSWD sign-extends the *low* word of each doubleword, and the guest builds a
   DMA address as `{0, ea}`. A doubleword spans two `_u32` slots, and since
   `_u32[i]` is SPU word `i` while the host packs `_u64` little-endian,
   `_s64[d]` holds the two words in the opposite order — so the old code read
   the *high* word and wrote the value back into the *high* word, extending
   `{0, ea}` to all zeros. Every PCM fetch targeted EA 0 and was discarded while
   the mixer faithfully scaled an empty buffer. `xsbh`/`xshw` are *correct* as
   written (host-native layout already puts the low sub-element at the even
   index) — only `xswd` needed fixing, because only it routes through `_s64`.
   Covered by `runtime/spu/tests/test_spu_helpers.c`.

2. **The lifter dropped the continuation after a resumable `stop`.** For
   `stop 0x110` (`sys_spu_thread_receive_event`) that continuation is the four
   `rdch SPU_RdInMbox` draining the kernel's reply. Unread, the mailbox stayed
   non-empty and the guest gates its reply on `rchcnt(SPU_RdInMbox) == 0`, so
   the SPU could never answer and the PPU parked in the `END ` wait forever —
   never rebuilding its voice list. Fixed in `spu_lifter.py`; it affected **4 of
   5** images (5/12/72/7 dropped blocks). Fixing it is also what made the
   attract movies play.

3. **Missed lift entry points** — the mixer's DSP modules are called through
   function pointers, so `spu_lookup` failing on them dropped into the
   branch-to-0 handler, which sets `STOPPED_BY_HALT` and *returns*, continuing
   with corrupt state (observed: the 8192-byte output PUT retargeted to guest
   address 0). See the `--extra-funcs` list and registry map in `CMakeLists.txt`.

Architecture worth keeping: the mixer is a FourCC-keyed **DSP module chain**
(registry at LS 0x15310, stride 28, lookup at `0xCD50`) holding only *effects*
— MIX/MUX/PTCH/VOLP/IIRF/REVB/LHPF/DPL2/MONO/COMP. Sources are a *separate*
codec registry (count at LS 0x15300, entries from 0x15190, stride 40, lookup at
`0xC9F0`, handler at entry+0x14) holding PCM/PCML/WAV/AIFF/VAG/PCMF/AT3P/IS14.
Per-voice flow: build input descriptor -> call source codec -> effect chain ->
clamp -> PUT 8192 bytes. `func_00376EE0` builds the command list the SPU reads,
emitting `i | 0x20000` per voice whose `+0x1A8` or `+0x140` is non-zero and a
`0x00010000` terminator; voice table stride is `0x310`.

`kMaxSamples` in `taiko_atrac.cpp` must be **2048** (ATRAC3plus frame size);
RPCS3 shows the guest sizing its decoder ring to 0x4000 bytes / 0x800 samples
from `cellAtracGetMaxSample`.

Two silent failure paths were made visible while hunting this and should stay:
`mfc_do_transfer` now logs `BADSIZE` instead of silently returning on a bad
size, and DMA to the guest null page (EA < 0x10000) is rejected outright — the
mixer had been writing 8 KB to guest address 0 tens of thousands of times per
run, which predates the audio work.

Debug aids: `TAIKO_AUDIO_TRACE=1` gives `[audio-spu-dma]` (DMA shapes, deduped
per call site), `[audio-spu-out]` (mixer output peak, measured in local store
*before* main memory — this is what distinguishes "mixer produced silence" from
"the copy to cellAudio lost it"), `[cellAudio-pcm]` and `[taiko_atrac-pcm]`.
Bounded SPU instruction tracing: lift with `--trace`, then
`SPU_TRACE_FILE=<path> SPU_TRACE_TRIG=<hex LS pc> SPU_TRACE_LIMIT=<n>` — without
the trigger/limit the facility is unusable, since the SPU runs far faster than
realtime. `tools/guestmem.py PID ADDR [N]` reads guest memory big-endian; do not
hardcode `vm_base`, it changes per run.

### Streamed song audio (fixed 2026-08-13; cached 2026-08-16)

Catalog previews and selected songs are now decoded from the complete RIFF
embedded in their `.nub`. `cellAtracSetDataAndGetMemSize` often receives only
an 8192-byte prefix, and later `cellAtracAddStreamData` writes are a circular,
frame-aligned buffer. The old shim concatenated those writes as if they were a
linear file; on `SONG_MIKUGV` it skipped 1.59 MiB, so FFmpeg emitted about 20
seconds of PCM followed by corruption and an early cutoff. Do not restore that
incremental-prefix scheme.

`taiko_atrac.cpp` now uses the statically linked, minimal FFmpeg 8.1.2 build to
decode in-process. It matches the prefix against `data/sound/bgm/nub/*.nub`,
decodes the complete embedded RIFF, and validates PCM duration against the RIFF
`fact` sample count. `SetData` does not return until PCM is ready: this can add
selection/setup latency, but the game never advances through fabricated silence
while an external decoder catches up. Later guest stream reads are drained
through a bounded circular write pointer but are not used as decode input.

FFmpeg only manufactures source-rate stereo PCM. Playback still flows through
`cellAtracDecode` -> the title's three-slot decoder ring -> lifted bnusCore SPU
mixer -> cellAudio, so the game's voice commands, cue resets, loop points,
44.1-to-48 kHz conversion, and output clock remain authoritative. There is no
Python helper, FFmpeg process/DLL, or direct host mixer.
Build the pinned MinGW libraries first with `scripts/build_ffmpeg_mingw.sh`.

Decoded PCM is cached on demand and shared between cellAtrac handles. This is
important for gameplay synchronization: Green first opens the selected song at
its preview cue, then opens the same RIFF again at cursor zero for gameplay.
Before the cache, `SONG_MIKUGV` repeated a 282.03 ms full-file FFmpeg decode on
the gameplay audio thread while the chart continued initializing; the cached
gameplay open took 6.88 ms. The full-RIFF-content cache applies to every ATRAC
song and jingle, not to VAG effects (those use the lifted SPU decoder). It is a
byte-bounded LRU, 512 MiB by default, so browsing the catalog cannot retain all
decoded songs indefinitely; `TAIKO_AUDIO_PCM_CACHE_MB` selects 64--8192 MiB.
The source-location index uses the stable first 4 KiB of the RIFF, allowing the
8 KiB preview prefix and the larger gameplay prefix to resolve to the same NUB.

`TAIKO_AUDIO_OFFSET_MS` advances only gameplay `SONG_*` PCM by the requested
number of milliseconds (0--1000, default 0). It does not affect catalog
previews, jingles, voices, or VAG effects. Positive values compensate for host
output latency by beginning the song farther into its decoded PCM; the value is
converted with the source sample rate and is reapplied if gameplay resets its
decoder to sample zero. Native Linux play testing found 60 ms comfortable on
the current PipeWire/device setup, but that is deliberately not compiled into
the executable or launcher because the appropriate value is output-device
specific. Example:

```sh
TAIKO_AUDIO_OFFSET_MS=60 ./run-taiko-linux.sh
```

Preview cues need no title-specific NSH parser in the host. Green reads the
`.nsh` itself and passes its absolute PCM cue as `uiSample` to
`cellAtracResetPlayPosition`; the shim must set the decode cursor to that sample,
not zero. Live validation scrolled several uncached songs, played
`SONG_MIKUGV` beyond its old corruption point, and confirmed that previews start
at their intended cues.

Looping is carried by the standard RIFF `smpl` chunk, not by a separate host
filename rule. The shim parses its start/end/play-count fields during SetData,
reports the loop through `cellAtracGetLoopInfo`, returns
`CELL_ATRAC_LOOP_STREAM_DATA_IS_ON_MEMORY`, and wraps at the authored end sample
rather than FFmpeg's padded PCM tail. Green's 38 looped NUBs all contain one
loop with play count zero (infinite); ordinary songs without `smpl` still stop.
`run-taiko.sh` enables `TAIKO_AUDIO_DECODE=1` and `TAIKO_AUDIO_SPU=1` by default;
explicit `=0` remains available for silent/headless diagnosis.

**Open audio issue:** playback is occasionally crackly during poor-performance
screens. Removing the mixer's post-submit pacing and relying only on WASAPI
backpressure made playback severely robotic, so that experiment was fully
reverted. The endpoint itself reported no underruns. `TAIKO_AUDIO_RING_TRACE=1`
now hashes each guest cellAudio ring block before/after consumption and reports
`[cellAudio-producer] RACE` (concurrent write) or `STALE` (the same non-silent
slot consumed again one ring later). The first restored-pacing trace observed
repeated `STALE` blocks and no `RACE` blocks around opening music; continue from
there after the broader frame-performance work. This diagnostic is opt-in and
does not modify playback data or timing.

## VAG sound effects (fixed 2026-08-12)

All audio now plays: BGM and Player Entry music (AT3P) plus sound effects,
Don-chan voice clips and the warning screen (VAG).

VAG was silent because of a **second lifter slot-convention bug**, the same
family as `spu_xswd`. `BRHZ`/`BRHNZ`/`IHZ`/`IHNZ` test the halfword in the
CBEA *preferred slot*, which is bytes 2-3 of the register. Our u128 is
host-native per 32-bit word (`SPU_W` reverses bytes within a word: SPU byte 0 ->
`_u8[3]`), so that halfword is `_u16[0]`; `spu_lifter.py` emitted `_u16[1]`,
i.e. SPU bytes 0-1. Every such branch inverted whenever the two halves differ --
**87 of them in the audio image alone**. One is the gate at `0x2CE4` in the VAG
decoder (`ceqbi` -> `xsbh` -> `brhz`), so the decoder skipped its decode path and
returned an all-zero buffer. Guarded by `test_preferred_slot_convention` in
`runtime/spu/tests/test_spu_helpers.c`.

How it was isolated, which is the reusable part: RPCS3 was frozen immediately
after the VAG codec init returned (breakpoint on `func_0037CC00`, then on its
return address taken from `LR`) and its source object compared with ours. They
were **byte-identical** -- same `+0x90` data, `+0x94` size, `+0x9C` size>>4,
`+0xA0` blocks*0x1C, same zeros elsewhere. Identical inputs with different
behaviour proved the defect was in our lifted SPU code, not in the PPU init, not
a missing data feed. Everything before that had been chasing the wrong layer.

Dead ends recorded so they are not repeated: `+0x0C`, `+0xA8`, `+0xB4` and byte
`+0xB3` are **zero on RPCS3 too** -- zero is normal, not a missing feed;
`YDKJ_AWATCH` on `+0xA8` and `SPU_LSWATCH=1FDA8` both show no writer;
`spu_mpy/mpyu/mpyh/mpyhh/mpyhhu`, `xsbh` and `xshw` are all correct for this
layout -- do not "fix" them (the existing tests catch it).

Oracle recipe: RPCS3 needs **PPU Interpreter (static)** for breakpoints.
Connecting halts the emulator, so sample by detach -> let it run -> reconnect ->
read. Ours base `0x42000000`, RPCS3 `0x32400000`, identical offsets. Do not
compute `src` from the voice index -- read `voice+0x2EC`; the pool is not a
1:1 index mapping. Voice stride `0x310`, format FourCC at `+0x1B4`, mixer ctx
at `0x1394140` (`+0xB0` voice table, `+0xB4` source pool, `+0xC8` command block).

## Reverse engineering: names and Ghidra

Lifted `func_00XXXXXX` **is** Ghidra address `0xXXXXXX` — same binary, no drift,
so Ghidra names transfer 1:1. `meta/names.json` is the store (1609 entries,
`src:"derived"` marks ones inferred at runtime rather than from Ghidra).

The analysed program is **`EBOOT GREEN.elf` in the `taiko_headless` project**
(31,485 functions). The `taiko` project's `EBOOT.elf` has only 2 functions —
wrong one. Decompile headlessly:

```sh
G=/home/silvaluca/Desktop/ghidra_12.1.2_PUBLIC
"$G/support/analyzeHeadless" /home/silvaluca/Desktop/Taiko taiko_headless \
    -process "EBOOT GREEN.elf" -noanalysis \
    -scriptPath <dir> -postScript DecompAt.java 0x0037958C
```

**Enter from thread entry points, not from `main`.** `main` fans out into
thousands of functions on a boot path that already works; the thread bodies are
where the unknowns are. `sys_ppu_thread_create` logs every entry (an OPD — read
the code address out of it), which gives a finite worklist of ~10.

The 8 MUCHA service threads share body `func_001C9C34`, a dispatcher only:

```c
base = *(u32*)0x0102F708;  slot = base + idx*0x18 + 0x5c;  obj = *slot;
(*obj->vtable[0x08])(...);   // init      (services 0-3: func_00908138)
(*obj->vtable[0x0C])(...);   // exec loop (per service)
(*obj->vtable[0x10])(...);   // cleanup   (services 0-3: func_009081A4)
```

Resolved exec loops: Resident `00908244`, Delay `00908200`, Priority `009082D4`,
Versionup `00908290`, MuchaMain `0092D74C`, USBAuth `0092953C` (which the
project had already independently named `USBAsyncAuth_exec` — a good check that
the vtable layout reading is correct). The `[svc]` log line dumps this table at
runtime on the first lwmutex timeout.

## Lifter: do NOT bump without testing rendering

`ps3recomp`'s `fix/fold-merge-dropped-fixes` branch is 595 commits ahead and
carries real PPC correctness fixes (XER[CA], `rlwinm` sign-extension,
callee-save ordering, `ra=0` base, update-form writeback, jump tables). It was
adopted, re-lifted (37,536 → 41,854 functions) and **reverted**: its output
renders a black screen at 1 draw / 2 FPS and then locks up, while the old lifted
code renders fine on the identical runtime. That output is **not** in this
tree; it is kept in the `TaikoRecomp-archive` checkout under
`src/recomp.newlift/` for bisecting which fix breaks it — suspect jump-table
discovery and callee-save reordering first. `src/recomp/` is gitignored, so the
*committed lifter* is what regenerates it; that is why the revert matters.

Only the lifter generates code. Everything in `runtime/` and `libs/` is the
hand-written host side — that is where HLE bugs like the lwmutex no-op live, and
fixing them changes behaviour without touching a line of generated source.

## Bone matrices: vmrghw lifted as a halfword merge (fixed 2026-08-14)

**Root cause of the collapsed models.** `ppu_lifter.py` picked the vmrg element
size with `"h" in mn[4:]`, which also matches the high/low selector, so
**`vmrghw` (merge WORD) was emitted as a halfword merge** -- keeping only the
high halfword of each word. `vmrglw` escaped by luck (`"lw"` has no `'h'`).

It surfaced in the guest's 4x4 matrix inverse, `func_0051F8EC`, whose 12 merges
are all `vmrghw` (a transpose). Its output had columns 0-1 correct and columns
2-3 garbage/NaN; that matrix is the `+0x24` src array every bone matrix is then
multiplied by, so the corruption propagated to the whole skeleton -- collapsing
each bone to a line that still followed the animation.

Fixed in `ppu_lifter.py` (size = last character, `high = mn[4] == 'h'`), with
regression coverage in `ps3recomp/tools/tests/test_ppu_lifter_vmx.py`. Since
`src/recomp/` is a generated snapshot that is expensive and risky to regenerate,
**`tools/fix_vmrghw_snapshot.py`** repairs it in place: a buggy `vmrghw` and a
correct `vmrghh` emit *identical* C, so it disambiguates each site against the
guest disassembly (the lifter emits one merge per guest vmrg*, in order) and
cross-checks operands before rewriting. It reported 95 merge sites in 16
functions, rewrote 64, with **zero operand mismatches**. Re-run it after any
re-lift done with an unfixed lifter.

How it was found, since the method generalises: reading lifted code kept saying
"correct". What worked was **probes compiled into the generated source** --
snapshot the inputs, recompute in plain C, compare -- plus checkpoint tracing
that records v0-v19 at 10 points and dumps the trace only for invocations that
end corrupt. That named the first bad register (v19) and the exact region
between two checkpoints. `ctx->lr` at a helper's entry is the guest return
address and identifies the calling guest function exactly.

Watch out: matching a corrupt buffer by ADDRESS does not work -- these arrays
are reallocated at a different guest address on every model reload.

Result: bone matrices and skinned vertices are now correct (verified live: clean
identity palettes with `w = 1.0`, and output vertices like
`(-5.661, 3.235, 8.385)`). Don-chan's silhouette is correctly shaped and
oriented. The later intermittent one-frame explosions were a separate SPURS
job-lifetime defect, documented at the end of the model-skinning section.

## Model skinning: native SPURS job (2026-08-13)

Models (Don-chan and friends) rendered as a thin line because their vertices are
skinned by a **SPURS job chain** and `cellSpursRunJobChain` was a stub, so the
scratch vertex arena kept the guest heap's `0xCD` fill.

`ps3recomp/libs/spurs/cellSpurs.c` now exposes hook pointers —
`g_spurs_job_chain_runner` (handed the chain by `cellSpursRunJobChain`) and
`g_spurs_job_submit` (called at the exact guest command-publication store).
`g_spurs_job_chain_drain` remains only as an opt-in legacy diagnostic, and
`g_spurs_job_output_probe` maps bad draw EAs back to native jobs.
`src/taiko_vertex_job.cpp` claims the chain and runs 4-bone linear-blend
skinning natively; the math is `src/taiko_skin.h`, checked by
`tools/tests/test_taiko_skin.c`
(`cc -Isrc tools/tests/test_taiko_skin.c -lm`). `TAIKO_VERTEX_JOB=0` disables it,
`TAIKO_VERTEX_JOB_TRACE=N` dumps N jobs.

**`ppu_register_function` cannot hook this path.** It only intercepts calls that
go through `ppu_lookup`. `func_00529320` (the ring enqueue) is called directly by
C symbol from all 7 of its call sites, and its callers `func_0050E63C/7C4/B64/E98`
are reached via `g_trampoline_fn = (void(*)(void*))func_0050E7C4` in
`func_005C7ACC` — also a direct symbol reference. So "zero `func_X(ctx)` call
sites" does **not** mean a function is indirect-dispatched; grep the bare symbol
too. `--wrap` cannot help either, since definition and callers share a TU.
`ppu_set_project_register_hooks` now accumulates callbacks instead of replacing
the single slot `taiko_lumen.cpp` held.

Ring protocol and the descriptor layout are documented in the file's header
comment. Three traps worth repeating:

- The builder stores each scalar with a zero-extended `vm_write64`, so every u32
  sits in the **low half** of its 8-byte slot (`fmt` at `+0x54`, `count` `+0x5C`,
  `src` `+0x64`, `dst` `+0x6C`).
- A slot's command word is written only *after* its descriptor is memcpy'd in,
  so drain the command list, not the slot counter, or you race a half-written job.
- **The command list is not all jobs.** Besides job EAs (low 3 bits 0) and the
  jump-to-self pre-fill (3, meaning "reserved, not published yet"), it carries
  bare control codes such as `2`. Treating a control code as unpublished stops
  the drain dead: the chain ran 4 of ~60 queued jobs and most of the model was
  never skinned. Skip control codes; stop only on JTS and END.

The chain also contains a second job binary, **`0x00F73B00`** (0x890 bytes, no
DMA list, params look like SPURS-internal addresses), which is not implemented.
Unknown binaries are now named once in the log rather than dropped silently.

**Still open: the bone-matrix palette is not produced.** The palette is
per-frame scratch (its EA rotates every frame; sizes `count<<6`, 35 matrices of
64 bytes) and samples read back as all zeros or as uninitialised garbage with a
plausible float4 translation in row 3 and NaN rotation rows. RPCS3's equivalent
buffer — same address minus the 0xFC00000 io delta, ours `0x40100000` vs theirs
`0x30500000` — holds proper orthonormal matrices:

```
bcbfa04a 3f7fee10 394f2e7e 00000000   (-0.02339,  0.99978,  0.000197, 0)
bf7fed96 bcbf9e56 bb7bac13 00000000   (-0.99977, -0.02339, -0.003839, 0)
bb7b4ce7 b996a887 3f7fff83 00000000   (-0.003835,-0.000288, 0.99999,  0)
40fa700b 418a5122 3e0a4740 3f800000   ( 7.8262,  17.29,     0.13502,  1)
```

That diff is what proves the 4x4 row-vector reading is right and the defect is
upstream in guest code. With valid translations and broken rotations each bone
collapses to a line that still follows the animation, which is exactly what the
screen shows.

**The host skinning is VERIFIED CORRECT against RPCS3** — layout, indexing and
math. Recipe: break at `0x529320`, read `r4` (the 128-byte descriptor), then
break a second time with a memory condition on `r4+0x64 == <src EA>` so the
first job has completed and its `dst` holds finished output. Measured on Green's
Don-chan:

```
src vertex 0  pos(12.6671, 15.7754, 8.8843) bones(5,6,1,1) w(0.000306,0.99968,0,0)
RPCS3 output                     (7.68127, 4.20154, 8.96539)
ours, M[k] at ea + k*0x40     -> (7.68202, 4.17774, 8.96499)   <-- matches
ours, M[k] at ea + 0x40+k*0x40 -> (7.69262, 4.14273, 8.95674)
```

So bone `k` sits at DMA-buffer offset `k*0x40`, stride 0x40, row-vector, with
the translation in row 3 — exactly what `src/taiko_vertex_job.cpp` already does.
The all-zero block at offset 0 is just an unused bone 0, and the ~1e-3 residual
is one frame of pose drift (the captured `dst` is the previous frame's). A
buffer whose offset 0 looks like material colours
(`(0.9725,0.2824,0.1569,1.0)` = RGB(248,72,40)) belongs to a *different,
non-skinned* job — do not conclude from it that the layout is wrong.

**So the only remaining defect is the matrix data itself.** RPCS3's are clean
orthonormal rotations with the w column `(0,0,0,1)`; ours have **NaN in rows 0-2
and `w = 0` in row 3** where it must be `1.0`. That localises it precisely:
`func_0051E678` computes `out_row3 = A30*M0 + A31*M1 + A32*M2 + A33*M3`, so a
finite, plausible-looking translation whose `w == 0` means the *source* local
matrix `A` has `A33 = 0`, and rows 0-2 are NaN because `A`'s rows 0-2 are NaN.
The corruption is therefore in whatever produces the **local bone matrices** —
not in the traversal, the multiply, or the skinning.

**Narrowed further: a destination-offset defect, not bad math.** Our Don-chan
palette (job src `0x41A7A080`) against RPCS3's same buffer:

```
        RPCS3 (0x31b694c0)               ours (0x4176B1C0)
+0x40   1, 0, 0, 0                       1, 0, 0, 0                ok
+0x50   0, 0.95519, 0.04463, 0           0, 0, 0, 0                MISSING
+0x60   0, -0.04667, 0.99890, 0          0, 0.95519, 0.04463, 0    <- RPCS3's +0x50
+0x70   0, 0.19614, -0.21349, 1.0        0, 0, 0, 0                MISSING
```

Our `+0x60` is **bit-identical** to RPCS3's `+0x50`, so the arithmetic is right
and the row is written one slot too far: rows land at stride `0x20` instead of
`0x10` (`+0x40, +0x60, +0x80, +0xA0`), leaving `+0x50`/`+0x70` zero. The "NaN
rows" and `w == 0` that started this hunt are downstream of this one defect.

Writers were found with a gdb hardware watchpoint on a live palette word,
resolving the raw `$pc` through `nm build/taiko_boot.exe` (gdb cannot symbolize
the PE, so `info symbol` fails — resolve against nm the way `tools/hangstack.py`
does): `func_00542F48+0x359` and `func_00509274+0x6DA`. `func_00542F48` is the
palette append (push v28-v31, `FUN_0054575c` computes, `stvx` four rows, pop,
advance cursor by `0x40`) and **its lifted offsets are correct** (`+0`, `+0x10`,
`+0x20`, `+0x30` off `gpr[31]`). `func_00509274` writes the render struct's
palette end pointer (`+0x14`) and matrix count (`+0x1C`). Those two hits were on
a *different* buffer than Don-chan's, so the next step is to re-run the
watchpoint on the Don-chan palette specifically and name the writer using the
0x20 stride.

**Full chain, traced live (2026-08-14).** Each link was confirmed with a gdb
hardware watchpoint on the live buffer, resolving `$pc` through
`nm build/taiko_boot.exe`:

```
job DMA palette  <- func_00545E20 loop: verbatim 64-byte copy per bone,
                    r30 (source) / r26,r27 (two palettes, r27 = r26 + 0x880,
                    which is why the two DMA EAs are 0x880 apart)
source array     <- func_0051DBA4: "store v28-v31 to [r3]"
v28-v31          <- computed upstream  <-- CORRUPTION IS HERE
```

`func_00545E20`'s loop, `func_0051DBA4`, and `func_00542F48` all use correct
offsets (`+0`, `+0x10`, `+0x20`, `+0x30`) and correct increments (`0x40`), and
the generated x86 was disassembled to confirm it (`ctx` is in `%rbx` /`%rcx`;
`0xd0` = gpr[26], `0xe8` = gpr[29], `0x3e0` = vr[30]).

**The corruption signature, which is the thing to chase:** in the source matrix,
rows 0 and 2 are correct floats but rows 1 and 3 hold **16-bit values in the LOW
half of each word**:

```
row0  3f800000 00000000 00000000 00000000   (1, 0, 0, 0)           correct
row1  00003f80 00000000 00000000 00000000   0x3F80 = high half of 1.0f, low slot
row2  00000000 3f717f5c 3d5b2c0c 00000000   (0, 0.9433, 0.0535, 0) correct
row3  0000c170 00003be7 00000366 00000000   16-bit values, low slots
```

`0x3F80` is the *high* halfword of `0x3F800000` sitting in the *low* half. That
is the same wrong-slot family as the `spu_xswd` and `BRHZ` bugs. Odd rows are
affected, even rows are not.

Verified clean, don't redo: `stvebx/stvehx/stvewx` emission in `ppu_lifter.py`
(`slot = ea & (16-size)` is correct for the raw-BE register layout); the parent
transform entering both palette writers is never NaN (conditional breakpoints on
the v28 exponent, zero hits); `obj+0x110` is a valid scale/translate with
`w = 1.0`; v28-v31 at `func_0054575C` and `func_0051DBA4` entry is a valid
180-degree Z rotation; `func_0051E748` (`v28..31 = M x A`) reads and writes the
right registers; the matrix stack and per-thread TLS are healthy.

**Isolated to a single stable buffer (2026-08-14).** A conditional breakpoint on
`func_0051DBA4` (fires only when v29's first guest word has a zero high half and
non-zero low half) catches the bad matrix in flight. Its caller is
`func_002A5F5C`, which per bone does:

```
func_0051E5D0()            push matrix stack
r3 = r29; func_0051DBC4()  load v28-v31 from [r29]
r3 = r31; func_0051E678()  v28-v31 = A[r31] x v28-v31
r3 = r29; func_0051DBA4()  store v28-v31 back to [r29]
```

Dumping both inputs at the moment it goes wrong:

```
[r3]  = 0x40053140   1.0 at bytes 0, 20, 40, 60   <- perfect identity
[r31] = 0x40053e10   1.0 at bytes 0, 18, 36, 54   <- BROKEN, step 18 not 20
```

A 4x4 float identity puts `1.0` at byte `20*r` (16 row + 4 column). The broken
one steps by **18** (16 row + **2** column), i.e. a column index scaled by 2
instead of 4. Writing a 4-byte `1.0` at byte 18 is exactly what produces the
`00003f80` "halfword in the low slot" rows seen downstream — it is one float
store at a mis-scaled address, not a halfword or VMX bug.

`0x40053e10` is written **once** (a 100 s hardware watchpoint on it never
fires), so it is built at model/object load and then read unchanged every frame,
which is why the corruption is perfectly stable. `vsldoi` is ruled out: the whole
lifted image only ever uses SH 0/4/8/12, never the 14/12/10 the pattern would
need. `stvebx/stvehx/stvewx`, the copy loop, all three store helpers, the matrix
stack and per-thread TLS are all confirmed correct.

**Producer identified: the animation evaluator.** Arming a watchpoint at a fixed
address across a restart does NOT work -- these buffers are not allocated at the
same guest address run to run (`vm_base` is stable at `0x1468f0000`, the guest
allocation is not). Static tracing instead:

```
FUN_002a5f5c   skeleton update (holds lwmutex at +0x1f8), per bone:
                 FUN_0051e5d0()            push matrix stack
                 FUN_0051dbc4(dst)         load v28-v31
                 FUN_0051e678(src)         v28-v31 = A x M
                 FUN_0051dba4(dst)         store back
                 FUN_0051e5ac()            pop
               dst = *(obj + i*0x20 + 0x20), src = *(obj + i*0x20 + 0x24)
  -> src array is filled by FUN_002a588c, the ANIMATION EVALUATOR, which builds
     each bone matrix as:
                 FUN_0051dbc4(base + bone*0x40)   load
                 FUN_0051e9ac(kx, ky, kz)         translate by a keyframe
                 FUN_0051ee8c(int angle)          rotate
                 FUN_0051ede4(int angle)          rotate
     keyframes come from a stride-12 array (`p + 4 + n*12`).
```

Rotations use **fixed-point integer angles** through a sin/cos table:
`func_0051D8F4` (sin) / `func_0051D8E0` (cos, adds `0x4000` = 90 degrees) do
`r3 = (angle << 2) & 0x3FFFC; f1 = *(float*)(*(TOC+0x70B0) + r3)`. Both were
verified live and are **correct**: the table at `0x01407780` is properly
populated (`0.0, 9.587e-5, 1.9175e-4, ...` = sin(2*pi*n/65536)), and the
`rlwinm(x,2,14,29)` mask is exactly `index*4` over a 65536-entry table. The
`lvlx`+`vspltw` idiom these helpers use to get the scalar into a vector lane
also lifts correctly.

So translations (no table) come out right while rotations do not, which is the
observed symptom, but every individual link now checks out. Remaining
unexamined: the tails of `func_0051E9AC` (translate), `func_0051EE8C` /
`func_0051EDE4` (rotate), and the `func_0051E5D0`/`func_0051E5AC` push/pop pair
used by this loop (note: `0051E5AC`, not the `0051E3D8` used elsewhere).

**The animation helpers are CLEAN (2026-08-14).** Breaking at `func_0051E9AC`
(translate), `func_0051EE8C` and `func_0051EDE4` (rotate) and testing the
orthonormality of v28-v31 at each -- 401 samples across real rotations (angles
27, 33, 32768, negatives), not just the trivial zero-angle ones -- every single
matrix is orthonormal to 1e-3. So translate, both rotates, and the whole
animation-evaluator pipeline are correct. `func_0051E5AC` is just a wrapper
around the already-verified `func_0051E484` pop, so the push/pop pair is fine
too.

That rules the animation path out entirely and relocates the defect: the corrupt
array is the **`+0x24` (src) array**, while `FUN_002a588c` writes the **`+0x20`
(dst) array**. They are different buffers.

**The decisive comparison is the sign of zero.** RPCS3, same object, same
function (`break 0x2A5F5C`, obj = `r3`, index = `r4`, dst = `*(obj+idx*0x20+0x20)`,
src = `*(obj+idx*0x20+0x24)`):

```
RPCS3 src (0x300557e0)   3f800000 80000000 80000000 80000000   1.0 and -0.0
                         80000000 3f800000 80000000 80000000
RPCS3 dst (0x30046d30)   3f800000 00000000 00000000 00000000   1.0 and +0.0
ours  src                3f800000 00000000 00000000 00000000   1.0 and +0.0
                         00003f80 00000000 00000000 00000000   <- 1.0 at byte 18
```

RPCS3's **src** zeros are `-0.0` (`0x80000000`) -- the vmaddfp additive-identity
idiom the VMX matrix routines use -- while its **dst** zeros are plain `+0.0`.
Ours has `+0.0` in the src array. So our src buffer was never produced by the
VMX matrix path at all; it carries the signature of the plain-identity writer,
with its diagonal at byte step **18 instead of 20**.

Chasing the writer by address does NOT work and cost several runs: these buffers
are reallocated at a different guest address on every model reload (observed
`0x40051070` -> `0x40047180` -> a third), so a watchpoint armed on one is dead
the moment the model reloads. Value-conditional breakpoints on `func_0051DBA4`
do fire, but every corrupt store comes from one site, `func_002A5F5C+0xbe6`,
which is the propagation loop (`dst = src x dst`), not the origin.

**The corruption is an exact, uniform transform -- this is the fingerprint to
match.** Every row of a corrupt matrix is the four source floats' **high
halfwords packed into the first 8 bytes, with the upper 8 bytes zeroed**:

```
correct row (0, 1.0, 0, 0)    high halves [0000, 3f80, 0000, 0000]
  -> ours   00003f80 00000000 00000000 00000000
correct row (0, -15.0, 0, 1.0) high halves [0000, c170, 0000, 3f80]
  -> ours   0000c170 00003f80 00000000 00000000
correct row (1.0, 0, 0, 0)    high halves [3f80, 0000, 0000, 0000]
  -> ours   3f800000 00000000 00000000 00000000   (looks correct by luck)
```

All four rows of every sampled matrix fit this exactly, which is why the
diagonal appears to walk at byte step 18 instead of 20 -- that was a symptom of
this, not the cause. Note row 0 is indistinguishable from correct, which is why
even rows looked fine.

`result[h] = high_halfword(src_word[h])` for h in 0..3, rest zero, is precisely
what a **pack instruction that selects the halfword positionally instead of by
value** produces on our raw-big-endian `vr` layout (`vpkuwum` must take the LOW
halfword of each word; the positional index 0 of each word is the HIGH one).
That is the same slot-convention family as `spu_xswd` and `BRHZ`.

BUT the lifter implements only `vpkshss`; every other `vpk*` falls through to the
default, which emits a `/* TODO: ... */` comment (a silent no-op). And the
generated code contains **no** instruction-shaped TODOs at all -- all 29336 are
`.word` data sitting in `.text`. So no `vpk*` is being skipped and the guest is
not using one here. The operation is real but its source is still unidentified.

**Next step:** find what performs that pack. Since it is not `vpk*`, the
candidates are a `vperm` with a mis-built control vector, or an `lvsl`/`vperm`
unaligned-store idiom whose shift is off by 2. Grep the producing function for
`vperm` and dump its control vector live -- the control vector will read
`00 01 04 05 08 09 0c 0d ...` if this is the pack, which names the bug
immediately.

Verified NOT the cause, so don't re-derive them:

- `FUN_0055f134` walks bones and calls `FUN_005c794c` (trampoline to
  **`FUN_0051e678`**) with one matrix slot at
  `*(u32*)(param_3+0x14) - count*0x40 + bone*0x40`; `+0x14` points *past the
  end* of the palette and `+0x1c` is the matrix COUNT, not a byte size.
- `func_0051E678` is `v28..v31 = A x M` and has 4 vector loads and **zero
  stores** — the current transform lives in registers. Its lifted output was
  read instruction by instruction and is correct, including the `vspltisw 1` +
  `vslw` → `-0.0` splat used as the vmaddfp additive identity.
- The current matrix is pushed/popped by `func_0051E518`/`func_0051E484`, whose
  descriptor is in TLS at `r13 - 28652`: `{u32 top; u32 base; u32 count; u32
  capacity}`. Live main-thread values were healthy (`top=0x0126ca30
  base=0x0126c9f0 count=13/14`) with a valid `identity + translate(-168, 239)`
  on the stack.

**Per-thread TLS (fixed 2026-08-13.)** `ppu_loader.cpp` used to give every
`sys_ppu_thread_create` thread the same `PPU_TLS_TP` ("share main TLS for now")
while the main thread got its own block from `sys_initialize_tls`. Since the
guest keeps a per-thread matrix stack in TLS, all threads shared one stack
cursor. `ppu_tls_alloc()` in `ppu_loader.cpp` is now the single allocator for
both callers (so their bump pointers cannot overlap) and each thread gets its
own block copied from the PT_TLS template. Verified not to regress boot — but
it did **not** change the rotations, so it was a latent race, not this bug.

Note VMX stores memcpy straight to `vm_base`, so AWATCH/FLOW_WVAL cannot observe
the writer; gdb `watch` only fires on a value *change*; gdb `awatch` produced
nothing; and the palette EA rotates every frame so captured addresses go stale.
All dead ends, already paid for.

`tools/ghidra/DecompAt.java` is the headless decompiler script the AGENTS.md
Ghidra recipe refers to; it takes one or more guest addresses.

## Intermittent model explosions: delayed SPURS jobs (fixed 2026-08-15)

After the VMX matrix fix, Don-chan still exploded or lost faces for exactly one
frame at repeating intervals. Fill and outline followed the same bad silhouette,
so this was the shared skinned vertex arena, not culling or a fragment-shader
artifact.

RenderDoc provided the decisive layer boundary. In clean stepped captures,
Don's fill draw (for example event 1446 in `taiko_frame4156.rdc`) contained an
object-space triangle edge around 350 units where clean frames maxed near 3.9;
one input position was `(0.5, -0.5, 359)`. The post-VS output faithfully carried
that bad position. Static indices and UVs were unchanged, and the renderer's
source hash stayed stable across its CPU upload. Therefore Vulkan/D3D12, vertex
ordering, and a renderer-time read/write race were all ruled out: bad positions
already existed in guest memory before the draw.

`TAIKO_VERTEX_RACE_TRACE=1` adds three correlated diagnostics:

- `[VTXBAD]` identifies extreme indexed triangles and records copied/current
  source positions plus a before/after source hash.
- `[SKINPROBE]` maps each bad EA to the most recent native skin job and compares
  that job's completion-time output hash with current memory.
- `[SKINBAD]`/`[SKININPUTRACE]` dump the first extreme output, contributing
  bones/matrices, and palette/source hashes before and after execution.

The ledger proved the RSX uploader was reading stable data. It also caught
completed skin jobs whose matrices were plainly recycled memory: zero rows,
NaNs, translations over 1000, and outputs such as `(924, 34, 1674)`. Those
matrices were stable during the native job, meaning this was not a palette
being modified halfway through a copy; the job descriptor itself was too old.

**Root cause:** native skinning was delayed until
`cellGcm_rsx_process_fifo`. Green's producer uses a double-buffer ring and
promptly recycles both its 128-byte descriptors and the DMA-list palette memory.
A cursor of only `{buffer, slot count}` cannot identify a generation. The guest
can cycle A -> B -> A between host drains (an ABA problem), so delta polling
skipped the prefix of a new generation. Replaying every visible slot was also
wrong: it re-executed retained commands after their descriptor/palette storage
had been reused. The first case mixed old and new pose chunks; the second made
the entire model disappear and dropped performance to about 8 FPS.

**Fix:** execute exactly once at publication. `func_00529320` first copies the
descriptor and then publishes its EA with a 64-bit command store. Immediately
after that store, generated code calls `g_spurs_job_submit`; the title hook runs
the native skin job synchronously while all referenced storage is valid. The
normal RSX-time ring drain is disabled. `TAIKO_VERTEX_JOB_RING_DRAIN=1` restores
it only for diagnosis and must not be used normally.

The lifted snapshot is gitignored and direct calls bypass
`ppu_register_function`, so `tools/fix_vertex_submit_snapshot.py` applies this
narrow injection mechanically. CMake runs it at configure time and fails closed
if the expected publication-store shape changes. Re-run it manually after a
re-lift if diagnosing generation output.

Live validation showed Don-chan stable for several minutes with zero
`[SKINBAD]` or `[SKININPUTRACE]` events. A subsequent clean Release run used
`-O3 -DNDEBUG` globally (`-O2` remains explicitly selected for the enormous
lifted TUs), with all tracing/profile/capture switches unset. The normal
`run-taiko.sh` no longer enables the old `RTT_SAVERT`, `RTT_SAVEA`, or
`GCM2D_PROBE` Player Entry diagnostics.

**Performance remains open.** The traced 8 FPS run is not representative—it
hashed every job/vertex buffer and scanned indexed triangles—but the clean run
is still slow enough to require a separate profiling pass. Do not enable
`RSX_PROFILE`, `PPU_SAMPLE_PROFILE`, `TAIKO_VERTEX_RACE_TRACE`, batch capture,
or RenderDoc when establishing the clean baseline.

RenderDoc workflow retained for future GPU bugs:

- `run-taiko-renderdoc.sh` enables the Vulkan capture layer and writes captures
  under `build/renderdoc-captures` (override with `RENDERDOC_CAPTURE_DIR`).
- F11 toggles pause-after-present; F12 captures and advances one frame. F2—not
  F11—toggles the old Don body depth-always diagnostic. The earlier F11 conflict
  itself made Don's outline shell render through the body and produced false
  "corrupt" captures; frames 7038/7170 from that run are not evidence.
- Closing the visible Wine window can leave `taiko_boot.exe` alive. Before any
  relaunch, check the host with `pgrep -af 'taiko_bo[o]t'`, terminate the exact
  lingering PID, and verify it is gone.

## Lumen layer ordering (fixed 2026-08-13)

The game already provides the authoritative order. `func_0050C0C8` appends
`{command pointer, float sort key}`, `func_005096B8` forwards the key in `f1`,
and `func_0050BAD4` stable merge-sorts ascending (`left <= right`). RPCS3 then
executes that RSX stream in order. Do not enable `RTT_UNREVERSE`, globally sort
nested Lumen groups, or use depth as a substitute for this mechanism.

The recomp's apparent reversed ties were a PPU VMX lifter bug: `lvebx/lvehx/
lvewx` zeroed the destination and loaded every element into byte zero. In the
game's `lvsl -> lvewx -> vperm -> vspltw -> vcfux -> vrefp` sequence around
`func_00515354`, a non-zero EA slot selected zero, reciprocal became infinity,
and later layer/bounds math became NaN. Song Select measured 367 NaN keys out
of 398; after correcting EA-selected element placement it measured zero.

The source fix is in `ps3recomp/tools/ppu_lifter.py`; regression coverage is
`ps3recomp/tools/tests/test_ppu_lifter_vmx.py`. The current generated snapshot
was patched mechanically, but `src/recomp/` is gitignored, so future output
must be regenerated with the fixed lifter. Full analysis and RPCS3 evidence are
in `docs/layer_ordering_fix.md`.

Song Select still exposes a separate 3D geometry-input problem. RPCS3 captures
populated float3 position arrays for the indexed 600x600 passes; the recomp
decodes the same index stream and location/format semantics but reads `0xCD`
from the referenced position slots while UV data is valid. This is upstream of
D3D12 ordering/address decoding and must not be hidden with another sort hack.

## Transition rainbow / unsupported texture formats (fixed 2026-08-13)

Screen transitions rendered as a **giant white square** instead of the rainbow
wipe. The cause was a missing texture format, not anything about the wipe.

The wipe is three layers: two 1280x96 gradient strips (its leading and trailing
edges) and, between them, one quad whose texcoords run `0..1280 x 0..8` — a
**1280x8 rainbow strip stretched over the whole screen**, which is where the
eight vertical colour bands come from. Its `c257` Y scale is 100x the usual
one; that is the vertical stretch, not a bug.

That strip is `CELL_GCM_TEXTURE_D8R8G8B8` (**format `0x9E`**): A8R8G8B8's byte
layout with the alpha byte ignored, sampled as opaque. `0x9E` was absent from
`d3d12_bind_texture`'s supported list, so the `else` branch **memset the whole
texture unit** and the draw reached the fragment shader untextured. `0x9E` now
takes the A8R8G8B8 path in all five places (bind, upload eligibility, upload,
SRV format, hash size) with alpha forced to `0xFF`.

Reusable lessons:

- **A dropped binding looks identical to a draw that was never textured.**
  The lesson generalises past the tooling that found it: when something renders
  as a flat colour or not at all, first establish whether the backend was
  *given* a texture and discarded it, rather than assuming the guest never
  bound one. An unsupported format silently dropped at bind time was the cause
  here.
- **Do not make an unset texture unit sample white.** That workaround was
  tried first and produced the white square: it converted an invisible draw
  into an opaque full-screen one. It is retained only as a fallback (a null
  SRV samples transparent black and erases the draw), with `FP_NOWHITE=1` to
  turn it off. A draw landing on it means a binding was lost.
- The 640x720 patterned panels (`0x83B3780` green, `0x890B780` pale blue) are
  ordinary background tiles, drawn as ops 00-02. They are **not** the rainbow;
  do not chase them. RPCS3's local memory holds byte-identical data at the same
  addresses, which is a quick way to confirm a texture asset is not the suspect.
- `TEX_RAW_DUMP=1` + `tools/bc_decode.py` decodes guest textures to BMP. A
  montage of a whole capture's textures answers "what is this draw supposed to
  look like" faster than any amount of trace reading.

## Log-reading gotchas

- `build/taiko.log` contains NUL bytes (interleaved wide prints) — use
  `grep -a`. Multi-thread prints interleave mid-line.
- The log is overwritten per run.
- Compare against real hardware: the user has the PS3/RPCS3 side for reference
  screenshots; RPCS3 GDB and PS3 TMAPI MCP tools are available for oracle
  debugging of the same title.
