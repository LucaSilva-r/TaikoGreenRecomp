# Player Entry TAIKO+ handoff

TAIKO+ is an independent item in the stock Player Entry mode carousel. It
keeps the game's native drum navigation, focus animation, confirmation, fade,
and Player Entry cleanup. It does not place a host overlay over Player Entry,
and it does not replace Green's AI Ghost Battle item.

## Lumen patch

The clean `entry/packeddata.ddp` contains a Campaign carousel timeline inherited
from older versions but not used by Green's mode list. The patch deliberately
turns Campaign into the new item. Its stock label is string index 289 at frame
30. Training is index 288 at frame 20; Shop is index 290 at frame 40.

- appends `SetEntryPCMode` to the carousel unconditionally, after Green's
  conditional AI Battle insertion;
- replaces Campaign's frame-30 label with existing string index 691,
  `SetEntryPCMode`, in the normal, focused, and unavailable timelines;
  Training and Shop labels remain intact;
- replaces only `img00277.nut` and `img00285.nut`, Campaign's normal and
  focused labels, with FreeType-rendered `TAIKO+` art;
- makes Campaign's `img00278.nut` placeholder bubble transparent while
  retaining its original dimensions and packed size;
- raises `ModeSelect.BOARD_MAX` from four to five and clones authored `board0`
  as `board4` at depth 1000. Each Init removes/recreates only the dynamic clip,
  BEFORE induction-variable setup at original record 97 offset `0x585`, not
  inside the loop at `0x592`. The original loop populates `mcBoard[i]` from
  `this["board" + i]`. After its final `Tween_Move()`, the patch aligns
  `board4._y` to `board0._y`. Original controllers stay untouched;
- leaves `GetMode` and both mode switches stock: an unknown display label
  already returns numeric `ModeSelect.MODE_GAME` (zero), the Play route;
- aliases the custom display label to Play's label only in `FinishDecide`'s
  temporary switch value (record 97 `0xD42`). Unlike `GetMode`, this function
  has no Play fallback: unknown labels return without starting the fade.
  The actual board label is retained. A private boolean on `CppConnection`
  records this choice and resets on actual carousel initialization;
- tags only the new choice's existing final native `SetNextScene` call with
  a fourth integer argument `99`. The original three arguments are unchanged;
  stock choices use the unchanged three-argument call. It does not deliver an
  extra event from `GetMode` or call `NotifyModeSelectEnd` directly;
- updates the LMB record/body sizes, branches, and DDP LM size metadata.

The intended result has TAIKO+ for anonymous and card-authenticated entry. AI Battle
still depends on `current.IsAvailableBattle()`, so a card profile that enables
it gets both items: the original AI Battle button plus the new TAIKO+ button.
All AI Battle bytecode and artwork are unchanged.

Training's timeline, BanaCoin badge, dynamic counter placements, and textures
remain byte-for-byte stock. Removing layout tags from those sprite definitions
without rebuilding Lumen's internal layout metadata desynchronizes later board
states and must not be used as a shortcut.

Do not modify `img00280`/`img00287`/`img00313`: those belong to AI Battle. Do
not modify `img00279`/`img00286`: those belong to Reward Shop. Do not modify
`img00371`/`img00372`: they are shared Player Entry scene art, including the
512x640 right-side fairground background. The first attempted integration
overwrote unrelated assets, causing the giant pre-entry mode art and the
renamed Shop button.

## Runtime distribution

Shipping builds do not replace or edit the user's
`entry/packeddata.ddp`. `assets/entry_pc_mode.tkovl` is a compact binary delta
embedded in the executable. When the game opens the Player Entry archive, the
cellFs VFS reads the untouched file, validates its exact size and CRC32, and
applies the delta to a private delete-on-close stream. `cellFsOpen`,
`cellFsFstat`, and pathname-based `cellFsStat` therefore all see the patched
archive, while the file in the dump remains byte-for-byte original.

The supported clean Green archive is 12,305,848 bytes with CRC32 `f3aa0b97`
and SHA-256
`daa356e31fa9728ac558fc805119a11f957a3d8add54a58361adf5db671aa3ff`.
The virtual result is 12,306,204 bytes with CRC32 `fcb68d0d`. An unsupported
archive is logged and opened unchanged; an archive that already contains this
exact patch is also used unchanged. Set `TAIKO_ENTRY_PC_MODE_OVERLAY=0` to
disable injection explicitly.

The distributed `.tkovl` contains source-copy commands plus only literal
changed/inserted bytes, not a replacement game archive. Regenerate it after a
deliberate Lumen patch change with:

```sh
python3 tools/lumen/patch_entry_pc_mode.py \
  --source game/vfs/data/lumendata/packed/entry/packeddata.orig.ddp \
  --packlist game/vfs/data/lumendata/packed/entry/packlist.txt \
  --font fonts/font.ttf \
  --output /tmp/entry_pc_mode.ddp
python3 tools/make_overlay_patch.py \
  game/vfs/data/lumendata/packed/entry/packeddata.orig.ddp \
  /tmp/entry_pc_mode.ddp assets/entry_pc_mode.tkovl
```

The overlay generator replays its serialized result and refuses to write it
unless it recreates the target exactly.

For development, a patched archive can still be produced directly from a
known-clean copy:

```sh
python3 tools/lumen/patch_entry_pc_mode.py \
  --source game/vfs/data/lumendata/packed/entry/packeddata.orig.ddp \
  --packlist game/vfs/data/lumendata/packed/entry/packlist.txt \
  --font fonts/font.ttf \
  --output game/vfs/data/lumendata/packed/entry/packeddata.ddp
```

The script checks the clean Green action-record and string-pool signatures,
all three exact timeline labels, and each replacement texture size before it
writes anything. It intentionally rejects an already-patched archive; always
point `--source` at the clean backup.

For a deliberately obvious transform probe, add `--debug-y-probe`. It assigns
`board0._y - 100` to the fifth controller after layout. This is not a
shipping setting: it exists only to probe the visible
TAIKO+ panel.

## Native handoff

The fifth controller clones `board0`; only its display label is TAIKO+.
`GetMode` remains unmodified, including its normal-Play fallback at record 97
`0x1FB2`. The confirmed live native boundary is
`CppConnection.SendResultInfo` -> `Lumen.SetNextScene` (record 65 `0x5B37`),
which calls `func_002287BC` with `(nextScene, selectCampaign, decidedCampaignID)`.
The previous `Proc_Mode` / `NotifyModeSelectEnd` interception never logged a
callback during either live retry; do not treat it as a validated exit route.
PlayerEntry record 98 is now entirely stock again.

`ModeSelect.FinishDecide` records whether the display label is Taiko+ in
`CppConnection.SetEntryPCMode`, while aliasing its temporary label for Play's
fade. The next real Init clears that flag. `SendResultInfo` preserves the three
original arguments and appends `99` only when the flag is true. The existing
lifted hook at `func_002287BC` reads argument four (raw integer type 3), marks
Taiko+ pending, and leaves all arguments/memory and native commit code unchanged.
The native implementation reads only arguments 1..3. Functional marker handling
runs regardless of trace settings. No extra native event or callback is emitted.

The native `{name, OPD}` binding table begins at `0x00F93B5C`. Reading it from
`0x00F93B60` instead pairs each OPD with the NEXT name, giving plausible but
wrong callback names. Correct entries include:

| Native name | Function |
| --- | --- |
| NotifyModeSelectEnd | `0x0022426C` |
| NotifyBnCoinUseResult | `0x00226888` |
| SetNextScene | `0x002287BC` |
| SetPlayerData | `0x00223F74` (empty) |
| StartTimer | `0x00224B04` |

Raw AVM tags are also distinct from converted native callback tags:
`func_00397C04` converts raw integer 3 to native integer 2, and raw boolean 2
to native boolean 1. The hook reads raw stack cells, not converted values.

The verified Player Entry dispatcher hook waits for state 39/40, after the
stock fade and cleanup, before arming the handoff. With
`TAIKO_PLUS_STANDALONE=1`, the beginning of destination factory
`func_001FE470` runs its original outgoing-scene removal prefix and returns
before allocating either Song Select variant. The later
`SequenceController::push_task` interception remains a guarded fallback. With
the flag unset or zero, the factory and task push run normally, preserving the
validated GameSongSelect-backed diagnostic path. Other destinations and task
pushes remain untouched.

PC Mode then freezes the arcade sequence controller, gives input ownership to
the host frontend, and displays the host song-browser shell. Stock arcade mode
selections remain untouched.

The standalone lifecycle and its current native ownership gate are tracked in
[taiko_plus_runtime_plan.md](taiko_plus_runtime_plan.md). The legacy diagnostic
path still uses a live stock Song Select manager; standalone launch remains a
structured browser-safe failure until the manager/player/GameEnso ownership
chain is live-proved.

## Interactive verification

The first September 6 live retry still confirmed without fading and logged no
native mode-completion callback (`taiko-plus-lumen-audit-01.log`). It missed
the display-label switch in `FinishDecide`. The follow-up repair passes an
additional regression that fails on that previous patch: custom confirmation
installs Play's fade, and ten stock fade ticks publish `isFinish=true`.
The user then confirmed that Taiko+ reaches stock Song Select. That validates
confirmation/fade, not host activation. `taiko-plus-finish-decide-01.log`
reached states 39/40 without a pending marker. The corrected callback address
and raw type check still produced no callback in `taiko-plus-native-binding-01.log`;
that run again logged the three-argument SetNextScene commit. The fourth-argument
SetNextScene route is now live-validated for opening the custom menu: the user
confirmed it, and `taiko-plus-scene-commit-01.log` records count 4, marker 99,
state-39 handoff arming, and standalone host activation. This run used the
later push-task fallback with an already-allocated task; it does not validate
zero Song Select construction or native ownership. Gameplay remains gated.
Run `python3 tests/entry_lumen_patch_tests.py -v` (also in
CTest). Format tests need no assets; stock semantic checks run only when the
local original dump exists. They exercise numeric mode resolution, repeated
Init clone ownership, exactly-once native calls, stock labels/records, and
branch/function-boundary relocation. They do not emulate Lumen animation.

1. Enter Player Entry anonymously. Confirm that TAIKO+ appears as a new item
   and that Reward Shop/background art remains stock.
2. Enter with a card that enables AI Battle. Confirm that AI Battle retains its
   original text and TAIKO+ appears as an additional fifth item.
3. Enter anonymously, then touch the card after the carousel appears. Confirm
   that its live rebuild adds AI Battle without removing or shifting TAIKO+.
4. Select AI Battle and confirm that it follows its normal arcade path.
5. Select TAIKO+ with a centre hit and check for:

   ```text
   [taiko_pc_mode] PC Mode selected in the stock carousel
   [entry-next-scene] callback=002287BC count=4 marker_type=3 marker=99 taiko_plus=1
   [taiko_pc_mode] Player Entry reached final state 39; handoff armed
   [taiko_pc_mode] suppressing post-Entry arcade task ...
   [taiko_pc_mode] host PC Mode activated ...
   ```

6. Verify that the host browser owns the screen and drum input, with no arcade
   Song Select running behind it.

## September 6 audit: invalid earlier conclusions

- F001 stores the valid empty string at index zero; lengths exclude a NUL
  terminator that participates in four-byte padding. The old parser discarded
  empty entries, shifting every name. For example raw 803 is
  `NotifyBnCoinUseResult`, not `NotifyModeSelectEnd` (804). Raw 700 is `_y`,
  not `posY`; raw 1446 is `board`, not `Tween_Move`.
- The previous early marker targeted the wrong method and used register 4 in
  a function declaring only two registers. Its lack of a native callback log
  could not establish that the clone failed to finish its animation.
- The purported normal-mode expression was actually
  `ModeSelect.LABEL_NAME_SRC[ModeSelect.MODE_GAME]`, a string. Returning it
  instead of numeric `MODE_GAME` makes both normal-mode comparisons fail.
- The old clone insertion was at the relocated loop header. Every iteration,
  including the final failed condition test, removed/recreated `board4`.
  `mcBoard[4]` consequently referred to a removed clip after initialization.
- Suppressing event 12 did not suppress the earlier side effects inside
  `func_00226888` (actually NotifyBnCoinUseResult); the private early-event/
  suppression experiment is removed. Its historical lifted hook is trace-only.
- Claims that `board3` had a proven campaign-specific callback defect were
  unsupported. Training's frame-20 label was also accidentally renamed; it is
  now preserved, and only Campaign's actual frame-30 label is replaced.
