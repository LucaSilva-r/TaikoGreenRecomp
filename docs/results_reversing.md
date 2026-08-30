# Results transition reversing

This document records the narrow Results seam used to return to the host Song
Select while retaining Green's native gameplay teardown and player/music
manager state. Results presentation remains entirely stock.

## Normal class and update method

The normal class has RTTI name `N4game14GameEnsoResultE`, typeinfo at
`0x00f92d10`, and primary vtable at `0x00f92c98`. The relevant per-frame
virtual method is `func_001EBFD0`.

That method first calls `func_001E690C`, which advances the normal Results
presentation. It then invokes the current presentation-state completion
predicate. Nothing is redirected until that predicate returns true.

After completion, the stock branch is:

```text
manager = results + 0x0c
played  = manager + 0x408
limit   = manager + 0x40c

if results+0x20 == 0 and played < limit:
    ++played
    func_001EBEB0(results, scene_owner)  // another song
else:
    func_001E31E4(results, scene_owner)  // session finished

common Results/Lumen cleanup
```

The first repeat experiment demonstrated the ordinary branch directly. After
song 1 of 2, Green returned to its stock Song Select and displayed `2曲目`.
This proved `func_001EBEB0` was the correct continuation operation and also
showed why hooking only the final-session routine was insufficient.

## Host redirect

`func_001EBEB0` is not a state write. It allocates a normal `GameSongSelect`
object of size `0xf6c`, copy-constructs it with the existing music manager,
queues it through the scene owner, and removes Results when accepted. The host
therefore keeps this operation intact.

Two guarded hooks implement the repeat loop when `TAIKO_HOST_FRONTEND=1`:

1. At the beginning of `func_001EBEB0`,
   `taiko_frontend_results_continue_tick` verifies the current object has the
   normal `GameEnsoResult` vtable, changes the frontend from passthrough to host
   Song Select, reloads the host catalog view, and shows the opaque browser.
   The original function then constructs and queues stock Song Select.
2. At the beginning of `func_001E31E4`,
   `taiko_frontend_results_end_override` applies the same vtable/phase guards
   and calls `func_001EBEB0` instead of constructing the session-finished
   destination. The continuation hook above performs the single host takeover.

The caller's common cleanup runs after either path. No Results timer,
presentation state, gameplay teardown, player record, or music-manager field is
fabricated. The played-song counter is also not reset or incremented by host
code; once the arcade limit is reached, each final branch is semantically
replaced by another native Song Select construction.

## Live validation

`build-linux/taiko-song-repeat-02.log` validates the ordinary continuation in
an authenticated BanaPassport session:

```text
native Song Select commit ... id=kr01 difficulty=3
open '/data/fumen/kr01/solo/kr01_m.bin'
host Song Select reacquired after Results results=43BF4740
native Song Select commit ... id=kim69 difficulty=3
open '/data/fumen/kim69/solo/kim69_m.bin'
open '/data/sound/bgm/nsh/SONG_KIM69.nsh'
```

The second song used the same manager address (`0x0fefeda8`) and required no
coin, Player Entry, or second card authentication. Its Results screen then
exercised the final-session branch at the arcade limit:

```text
host Song Select reacquired after Results results=4323CF80
Results redirected to host Song Select ... played=2 limit=2
native Song Select commit ... id=kr01 difficulty=3
open '/data/fumen/kr01/solo/kr01_m.bin'
open '/data/sound/bgm/nsh/SONG_KR01.nsh'
```

This third gameplay launch proves that the loop remains valid beyond Green's
configured two-song arcade limit. Both the ordinary and final-session Results
destinations are live-validated with the same authenticated player manager.
