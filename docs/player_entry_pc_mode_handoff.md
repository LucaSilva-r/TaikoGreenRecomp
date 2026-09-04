# Player Entry TAIKO+ handoff

TAIKO+ is an independent item in the stock Player Entry mode carousel. It
keeps the game's native drum navigation, focus animation, confirmation, fade,
and Player Entry cleanup. It does not place a host overlay over Player Entry,
and it does not replace Green's AI Ghost Battle item.

## Lumen patch

The clean `entry/packeddata.ddp` contains a Campaign carousel timeline inherited
from older versions but not used by Green's mode list. The patch deliberately
turns Campaign into the new item. Its stock label points at frame 20, whose held
visual is the preceding Training board; the patched label points at frame 30,
where Campaign is actually drawn.

- appends `SetEntryPCMode` to the carousel unconditionally, after Green's
  conditional AI Battle insertion;
- replaces Campaign's frame-20 label with the unique existing string and moves
  that label to frame 30 in the normal, focused, and unavailable timelines;
  Shop's own frame-30 label remains intact;
- replaces only `img00277.nut` and `img00285.nut`, Campaign's normal and
  focused labels, with FreeType-rendered `TAIKO+` art;
- makes Campaign's `img00278.nut` placeholder bubble transparent while
  retaining its original dimensions and packed size;
- changes the carousel's `mcBoard` from four to five and clones the fourth
  authored `Tween_Move` clip as `Tween_Move4` before the stock initialization
  loop. `ActionCloneSprite` requires the VM stack order `source, target,
  depth`; reversing those operands creates a detached Play panel and leaves
  index four without a valid controller. The clone uses display depth 1000,
  immediately above the authored carousel's 924--987 range, so it is not
  hidden underneath the stock panels. The stock controller rebuilds the list
  when card availability changes. Each rebuild removes only the dynamic
  `Tween_Move4` and recreates it before the stock initialization loop; keeping
  the first clone across that refresh leaves its nested board timeline hidden.
  After the stock initialization loop and its final `Left(0)` call, the patch
  reads the authored `Tween_Move0.posY` and assigns that exact value to
  `Tween_Move4.posY`; the carousel drives its board controllers through the
  `posX`/`posY` pair rather than the MovieClip wrapper's `_x`/`_y`. This avoids
  a guessed offset and prevents clone-time
  transforms from being overwritten by the normal layout. All authored
  controllers, including the anonymous-login `Tween_Move3`, remain untouched.
  Lumen serializes multi-value `ActionPush` operands in reverse VM stack order,
  so the injected dynamic lookups use the same
  `this, "Tween_Move", index` byte order as the stock
  `this["Tween_Move" + i]` expression;
- extends `GetMode` to return private sentinel `99` for that label;
- updates the LMB record/body sizes, branches, and DDP LM size metadata.

The result has TAIKO+ for anonymous and card-authenticated entry. AI Battle
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

Build and install the patched archive from a known-clean copy:

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
`Tween_Move0.posY - 100` to the fifth controller after layout. This is not a
shipping setting: it exists only to prove whether `posY` owns the visible
TAIKO+ panel.

## Native handoff

The added selection reaches `func_002287BC` as sentinel 99.
`taiko_entry_game_mode_callback_trace` records PC Mode as pending, then rewrites
the callback value to normal Play mode 1 before the original game callback sees
it. This lets stock Player Entry bookkeeping and teardown complete without
placing an unknown mode in the game's player record. AI Battle's own mode value
therefore cannot activate PC Mode.

The verified Player Entry dispatcher hook waits for state 39/40, after the
stock fade and cleanup, before arming interception of the outgoing
`SequenceController::push_task` call. Only that post-Entry task is suppressed;
other task pushes between selection and teardown are left alone.

PC Mode then freezes the arcade sequence controller, gives input ownership to
the host frontend, and displays the host song-browser shell. Stock arcade mode
selections remain untouched.

The current host browser is a shell. Launching gameplay without constructing
the stock `GameSongSelect` scene still needs a direct native `GameEnso` setup
path; the older host frontend's launch helper depends on a live stock Song
Select manager and is not claimed as complete here.

## Interactive verification

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
   [taiko_pc_mode] rewrote guest sentinel 99 to safe stock mode 1
   [taiko_pc_mode] Player Entry reached final state 39; handoff armed
   [taiko_pc_mode] suppressing post-Entry arcade task ...
   [taiko_pc_mode] host PC Mode activated ...
   ```

6. Verify that the host browser owns the screen and drum input, with no arcade
   Song Select running behind it.
