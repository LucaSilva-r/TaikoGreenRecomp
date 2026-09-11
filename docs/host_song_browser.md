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
still uses Green's five courses. [Local TJA songs](custom_songs.md) appear in
CUSTOM TJA, with on-demand conversion and native title textures. Importing osu beatmaps and their arbitrary
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

The player cards display the native animated P1/P2 Don-chans for joined players,
with P2's texture flipped horizontally. Browser entry reuses the session's character service returned
by `005C573C`; no Player Entry or Song Select controller is cloned. The setup
follows Player Entry `00232E24`: camera preset 4, idle animation `0x2D`, and
independent red/blue model variants. The browser reads each logged-in player's native profile and applies its
colors and costume parts through `007F9A9C`, including the special whole-body
variant. Guests use the native slot defaults. Changes wait for the model loader
and are applied once; profile pointers are reacquired after login can relocate
the player map.

The character service's normal traversal renders its transparent targets
(`002A4224` -> `002A406C`). `00298F34` selects the current final target key;
`00518768` resolves its color buffer, whose `+0xF8` texture descriptor supplies
the address and dimensions. The overlay publishes only those values under its
lock. SDL_GPU samples the matching persistent color surface directly, skipping
missing targets rather than showing a white fallback. The native camera's
transparent padding is included when sizing the portraits. Null/CPU-only UI
paths omit the portraits.

Joining plays `don_entry_in` followed by the idle loop. Starting a song plays
the corresponding `select1P_out`/`select2P_out` motion with follow-up `-1`;
native `0029BD88` holds its final frame instead of restarting the jump.
Portraits follow the outgoing panels' slide and fade until the handoff ends.
Gameplay takes ownership of the character service without a visibility reset;
returning initializes the browser settings again. Tests cover separate target
identities, service readiness, loader retries, join/departure requests,
participation visibility, P2 mirroring, and handoff cleanup.

## Green category skin

The top-level category screen now uses Green's original Song Select artwork:
patterned green background, Japanese heading, folder tabs and bevel strips,
navigation arrows, drum prompts, player badges, and the red/blue footer.
The selected folder expands in the centre of a horizontal carousel. Nine
categories are published around the selection with wraparound; existing rim,
keyboard, wheel, search, login, and join commands keep their behavior.
Folder spines use the existing Zucchini-derived short-title renderer: 56x400
textures, top-aligned ink, UTF-8, small kana, punctuation groups, rotated dashes,
and vertical compression only when needed. Each label is cached as one texture;
horizontal titles and counts retain the native-resolution text path. The shared
title renderer now serializes its public calls so browser labels and background
custom-song title generation cannot race over FreeType profiles. Native character
portraits sit above the bottom player nameplates. Song browsing now uses the same carousel, with a category header, yellow
selected card, native return-folder illustration, and course/rating preview.
Custom subfolder lists, search results, and expanded difficulty selection retain
the previous layout for now.

`src/taiko_menu_art.h` reads the unmodified
`$PS3_VFS_ROOT/data/lumendata/packed/song_select/packeddata.ddp` (defaults to
`game/vfs` when no root is configured). It parses the archive table and decodes
only the selected NTP3 textures using the portable BC3 decoder or ARGB conversion.
The asset IDs in `src/taiko_menu_layout.h` refer to Green's 788-entry packlist.
Folder tabs and edges lose their transparent alignment padding at load time.
Frames join the original left/right bevel strips with a repeated inside column,
preserving the artwork's top/bottom edges and gradients as width animates; no
hand-coloured horizontal borders or extra shadow rectangles are added. The tab draws last: its coloured raised section covers the frame's top edge,
so no horizontal seam crosses the tab. Medley uses the gold folder set,
osu! uses matching pink pieces, and Nijiiro uses the original rainbow frame.
The artwork is cached as owning RGBA payloads with a 32 MiB total limit and stable
GPU IDs. Missing assets use the coloured host primitives and dynamic labels.
No game artwork is embedded, modified, or copied into the repository.

Inspect the source assets with Pillow and save a GPU preview:

```sh
python3 tools/lumen/inspect_song_select.py --output /tmp/green-song-select
LD_LIBRARY_PATH=third_party/sdl-gpu-linux/dxc-v1.8.2502/lib \
  TAIKO_UI_TRACE=1 build-linux/taiko_browser_gpu_preview \
  1920 1080 /tmp/green-categories.bmp categories
```

The standalone preview has fixture counts and no guest character surfaces; live
Don-chans are only present in the game. Validation includes the native executable
build, GPU previews, malformed/truncated archive tests, both category wrap
boundaries, opposite-corner portrait placement and handoff, and existing browser,
text-cache, and host-frame tests.

Category selection also changes the original patterned background and shows the
corresponding character illustration. Spine outlines use a darkened category
colour; selected song/return titles remain black. The shared indicator archive
supplies the original front-facing drum face, red centre, cyan rim halves and
arrows (textures 2, 3, 5, 6). The Choose/Confirm hints alternate idle/highlight
states every 600 ms. This host timing approximates the supplied reference; it
does not execute the original Lumen timeline. The GPU visitor keeps the menu
animated, while the CPU fallback refreshes on its existing 16 ms cadence.
Preview modes `anime`, `songs`, and `return` cover these additions.

The patterned background moves left at an initial 12 logical pixels/second,
using absolute monotonic time and a repeating mirrored pair of 640px tiles.
This is an eyeballed speed, independent of display refresh; a full pattern cycle
lasts about 107 seconds. Player nameplates use indicator textures 212–215 and
234–235: original shadow/border, red or cyan upper half, large circular player
badge, and a tinted lower half with the name centred inside it.

## Opening a category

Category confirmation follows the supplied 53-frame, 890 ms reference: fade out
its illustration and count, raise and settle the folder outline, extend its
right edge while pushing the right-hand categories away, then reveal Return
and the song spines. The outgoing category rows are owned by the overlay and
survive repeated catalog publications without restarting the opening clock.
The timing was reviewed in the standalone SDL preview. Following visual review,
inner cards retain the accepted category layout's y=132–553 bounds; the folder
outline expands around them. Left-hand categories remain visible.

Regular category lists now begin with a selected Return card. Rims/arrow keys
move from Return to the first song; confirming Return closes the category.
Search results and nested custom-folder lists keep their previous behavior.
Course selection and per-player readiness are unchanged.

Replay the opening without starting the guest:

```sh
LD_LIBRARY_PATH=third_party/sdl-gpu-linux/dxc-v1.8.2502/lib \
  build-linux/taiko_browser_gpu_preview 1280 720 /tmp/opening.bmp opening 120000
```

Enter/R restarts, Space pauses, and Left/Right step at 60 Hz. The loop includes
one second on the category and holds the finished opening before repeating.
The last argument is the preview lifetime in milliseconds. `opening-frame 30`
instead of `opening 120000` saves the exact 500 ms confirmation frame through
the GPU path. The deterministic clock is compiled only into the preview.
No guest character surface is available in this tool.

Validation covers retained outgoing rows, publication stability, cancellation
on return to categories, custom-folder exclusions, functional Return navigation,
and the existing browser/player, overlay-cache and host-frame tests.

## Scrolling and backing out

Song entries carry their absolute browser position and total entry count,
including Return cards. Scrolling moves the folder's left edge and the outer
categories with that position, while the category tab stays centred. The right
categories become visible near the final entry. The selected spine slides,
waits closed, then expands into the yellow preview card; its title follows the
expanding card. Repeated publications retain the current movement, and rapid
input interrupts from the currently displayed position.

The rendered folder is capped to x=-96..1376 in the logical 1280px view. The
list's entry count does not create an arbitrarily wide polygon. One shared
closing animation handles every position: retain the centre card frame, fade
out song contents, bring both visible sides to the centre using one progress
curve, then restore the category illustration. Different travel distances
finish together. The category rows and outgoing songs are owned copies, so
publishing the new category state cannot erase the closing animation early.
Navigation or reopening can interrupt it.

The scrolling and three backing-out recordings supplied on 2026-09-11 are
30 FPS reference captures. The closing phases use about 167 ms for the outgoing
content fade and complete contraction at 400 ms. The blank centre remains
while the tab settles, then the illustration returns at 667–800 ms. The accepted card bounds remain y=132–553.

```sh
LD_LIBRARY_PATH=third_party/sdl-gpu-linux/dxc-v1.8.2502/lib \
  build-linux/taiko_browser_gpu_preview 1280 720 /tmp/browser.bmp scrolling 180000
```

The demo opens, scrolls and backs out repeatedly. Any navigation key takes
manual control: Left/Right or D/K browse, Enter opens/confirms Return, Escape
backs out, Home/End select the first/last Return, M selects a middle Return,
and R restarts the demo. `back-start`, `back-middle` and `back-end` replace
`scrolling` for isolated closing demos. This fixture exercises renderer states;
production input remains covered separately by `taiko_browser_tests`.

Regression tests verify absolute positions across row-window boundaries,
publication stability, bounded GPU draw geometry for a million-entry list,
retained closing rows, and simultaneous arrival of the two folder edges.

### Border transition frame analysis

The follow-up comparison isolates the supplied opening GIF's title and frame
at 1280x720 (the 1080p source is scaled down for measurement). The white J-POP
glyphs occupy approximately x=568–710 throughout: the title translates, it does
not shrink. Representative top/bottom glyph bounds are:

| GIF frame | Time | White glyph y bounds | Border behavior |
| --- | --- | --- | --- |
| 0 | 0 ms | 66–101 | Original black frame visible |
| 2 | 33 ms | 67–101 | Black frame gone; background visible |
| 4 | 67 ms | 55–89 | Blue outline fading in |
| 6 | 100 ms | 46–79 | Outline and title rising |
| 10 | 167 ms | 37–71 | Highest pose, blue outline opaque |
| 20 | 333 ms | 47–81 | Settled pose before horizontal extension |

The reconstructed host motion keeps its accepted card geometry, uses one source
frame before removing the black frame, then fades in the blue shell over four
source frames. The title retains its 38px host font size, rises 30 logical
pixels with quadratic ease-out, and settles down 10 pixels. Background slices
are disjoint: the title strip must not overlap a full-width body strip during
alpha blending, which previously darkened the centre twice.

Closing retains the same side contraction and content timing. Its final settle
continues to x=444–836, bottom y=549, with the title portion inset six pixels.
The shell remains opaque and becomes fully covered by the foreground category
card/tab; it is not faded out while still protruding. A frozen-clock raster
comparison verifies that removing this final shell changes no pixels outside
the card's still-fading contents. Another check verifies identical body colour
in all three horizontal sections at half opacity.

For reproducible GPU frame inspection, `opening-frames 54` writes 54 BMPs using
the output argument as a filename prefix. `back-start-frames`,
`back-middle-frames` and `back-end-frames` do the same for closing. The singular
`*-frame N` variants save only frame N; all frame numbers use 60 Hz timestamps.
These modes freeze the preview clock and render the production GPU draw path.

### Character render resolution

The SDL GPU backend renders the 600x600 character attachment chain at the
window's integer resolution scale (1x at 720p, 2x at 1080p, capped at 3x).
Guest coordinates, UVs and filter offsets are retained; attachment storage,
viewports and scissors scale together. The host browser samples the enlarged
surface directly. Resizing preserves colour attachments until the next guest
update. Capture seeds remain in guest dimensions.

`TAIKO_CHARACTER_RENDER_SCALE=1`, `2` or `3` overrides automatic selection.
Higher settings increase character GPU fill and attachment memory usage.
An offline costume capture was compared at 600x600 and 1200x1200; live resize
and performance on lower-powered devices still require validation.
