# Host song browser

The graphical launchers (`run-taiko-linux.sh` and `run-taiko-windows.sh`) default
`TAIKO_PLUS_STANDALONE=1` so selecting Taiko+ replaces stock Song Select. When
launching the executable directly, set this variable yourself. An explicit
`TAIKO_PLUS_STANDALONE=0` selects the legacy diagnostic path, which keeps stock
Song Select running beneath the host UI. Restart the process after changing it.

Songs expand into difficulty rows in the library. Category and song browsing
use one shared selection; opening a song gives each joined player a cursor on
its chart list. P1 is red and P2 is blue. Both markers remain visible when they
choose the same chart.

- Rims browse categories/songs, or move that player's difficulty cursor while
  a song is open.
- Right centre opens a category, opens a song, then marks that player ready.
  Play starts when every joined player is ready. P2 can play alone.
- Left centre closes an open song, clears readiness and restores shared
  browsing. Outside an open song it returns through the browser's back path.
- Arrow keys and the wheel browse; while expanded they move the most recently
  active player's cursor. Enter opens/confirms. Escape closes the song before
  clearing a search or returning to categories.
- Search, random selection and returning from gameplay clear the expanded
  selection and readiness. Changing one player's chart preserves the other's
  ready state. Joining another player clears readiness for both.

The left panel retains song details and join/ready status. Difficulty selection
lives entirely in the library. The header and all installed stock difficulties
remain visible together, even at the beginning/end of a category. Row movement
and selection use 180 ms quintic ease-out transitions; repeated publication
retains an in-progress transition, and settled frames stop rerasterizing.

The SDL_GPU path draws panels as native-resolution rounded rectangles and text
as cached transparent textures. Text is rasterized from the TTF at the drawable
pixel scale; it is not an enlargement of the 720p browser image. The first
version caches complete outlined text runs rather than individual atlas glyphs,
which keeps each label to one GPU quad. FreeType's vector stroker generates
native-size rounded outlines once per text/size. A synchronous visitor copies
new texture payloads while the title lock is held; the backend retains no borrowed
pointers. GPU textures are reused across animation frames, with 256 slots and
64 MiB retained between frames (currently referenced textures survive the frame).
The CPU cache has 256 entries and a 16 MiB bitmap bound.

The layout uses logical 1280x720 coordinates and a uniform scale derived from the
actual swapchain pixel dimensions. HiDPI is enabled. Resize/monitor-scale changes
rasterize text at the new size; fractional row positions remain fractional on the
GPU. A 3440x1440 target therefore has 2560x1440 content and 440px black sidebars.
Windowed host animations follow the display refresh rate independently of the
60 Hz guest clock. Static screens stop presenting once their animation finishes.
The direct-KMS path uses its existing fixed scanout target and pacing; this change
does not introduce a native-resolution KMS mode.

Small gameplay pills retain the CPU overlay path. Null/alternate renderers also
retain CPU host-frame snapshots. GPU initialization/resource failure falls back
to that path. Warm CPU snapshots measured about 1.1 ms versus the original
15–17 ms; warm GPU draw preparation measured 0.01–0.03 ms with zero texture
uploads in the standalone Vulkan preview. These are CPU preparation measurements,
not end-to-end frame times. First-use font rasterization/shader compilation and
scale changes still have a cold cost (roughly 40–55 ms for the entire preview).

Build `taiko_browser_gpu_preview` and run it from the repository root to validate
without a game dump. Optional arguments are width, height, and an output BMP:

```sh
LD_LIBRARY_PATH=third_party/sdl-gpu-linux/dxc-v1.8.2502/lib \
  TAIKO_UI_TRACE=1 build-linux/taiko_browser_gpu_preview 3840 2160 /tmp/browser.bmp
```

The preview runs for four seconds and saves through the actual GPU draw path.
`TAIKO_UI_TRACE=1` reports scale, draw count, preparation time, new text uploads,
and GPU text storage. It is diagnostic and should normally be unset.

Stars are authored Green ratings from `data/fumen/tuning.bin`. The binary
layout follows guest loader `func_007D5034`: a big-endian record count, 0x90c
bytes per song, then an offset-addressed string pool. A course record is 0x80
bytes, with its string offset at +0 and stars at +4. The `ex_<id>` record's Oni
rating belongs to `<id>_x.bin` (Ura). Missing or malformed ratings display
`★ --`; difficulty availability still comes from installed chart files.

The overlay receives explicit chart identities, ratings and player-marker masks
rather than assuming a fixed difficulty button position. Stock launch mapping
still uses Green's five courses. Importing osu beatmaps and their arbitrary
named difficulties is future work; this change establishes the expanded-list
interaction, using osu's `BeatmapCarousel` song-group/child-row structure and
`Panel` easing as references.

Validation: native `taiko_boot` build; `taiko_browser_tests` exercises production
frontend input with fixture catalog/guest/overlay boundaries; player-state and
catalog tests cover independent readiness, sparse courses, Ura metadata and
malformed binary bounds. The actual overlay renderer was also inspected with
the game font and checked for stable frame versions after animations settle.

Browser drum feedback uses the original `SE_COM.nub` Don/Ka samples: D/K and
Z/V play Ka; F/J and X/C play Don. Both players can overlap hits, and each
input produces one sound even when it also opens, closes, or changes a row.
Keyboard/wheel navigation uses the same feedback. The samples preload off the
input/audio threads, then play from a bounded 16-voice pool with the game's
menu volume. Missing/invalid bank data leaves feedback silent. Gameplay
handoff fades host voices out with the browser audio.
