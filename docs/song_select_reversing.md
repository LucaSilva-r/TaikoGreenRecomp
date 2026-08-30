# Song Select reversing

This document records the normal cabinet Song Select contract used by the
opt-in host frontend. The goal is to select stock content through Green's own
music/gameplay managers without automating or rendering the Song Select Lumen
timeline.

## Class identification

Two early probes targeted alternate modes and must not be used for the normal
cabinet flow:

| Address | RTTI | Meaning |
| ---: | --- | --- |
| `0x000d2208` | `game::GameGhostSongSelect` | Ghost mode |
| `0x000fa0c0` | `game::GameWaiwaiSongSelect` | Waiwai mode |

The normal class has RTTI name `N4game14GameSongSelectE`, typeinfo at
`0x00f93038`, and primary vtable at `0x00f92fc0`. Its relevant virtual methods
are:

| Address | Recovered role |
| ---: | --- |
| `0x001f7af0` | scene construction/entry |
| `0x001f5ca8` | per-frame state dispatcher |
| `0x001f5390` | exit/teardown |

The host hook is installed at the beginning of `func_001F5CA8`, which is a
main-PPU scene-local boundary. Input/UI threads only publish atomics; they do
not call guest functions or inspect guest objects.

## Live catalog mapping

The host catalog comes from `/data/other/musicinfo.xml`, filtered to chart files
actually present under `/data/fumen`. This is presentation metadata, not the
index accepted by the game manager.

The live music-manager pointer is stored at `GameSongSelect + 0x0c`. Its song
records are a contiguous vector:

```text
begin = manager + 0x434
end   = manager + 0x438
stride = 0x90
```

Each record has a four-byte header followed by a PS3 Dinkumware `std::string`
containing the music ID at record `+0x04`. For the observed small-string
representation, bytes are inline at the string-object offset, length is at
string `+0x10` (record `+0x14`), and capacity is at string `+0x14` (record
`+0x18`). When capacity is greater than 15 the string object's first word is
the character pointer. The unique music ID is at record `+0x1c` (for example,
`mikugv` was unique ID 875).

The host resolves its selected textual music ID against this live vector on
every launch. It never assumes XML order equals manager order.

## Native confirmation transaction

`func_001F5CA8` dispatches the scene state at `scene + 0x10`. State 10 is the
stock confirmation case. It resolves the current carousel item for
presentation, then constructs this semantic argument record:

```c
struct SongSelectionArgs {
    uint32_t live_song_index;
    DifficultySelection *p1; // scene + 0x20
    DifficultySelection *p2; // scene + 0x50
    uint8_t mode_flag;        // copied from scene + 0xf68
};
```

The selected difficulty is the 32-bit value at `DifficultySelection + 4`, in
the same order as chart suffixes `e/n/h/m/x` (0 through 4). State 10 performs:

```text
scene + 0x14 = 2
func_002456B0(scene + 0xc8)       // native confirmation callback
func_007FCE6C(args, music_manager) // authoritative selection commit
scene + 0x10 = 11
```

`func_007FCE6C` is the important boundary. It clamps both difficulty values,
looks up the `0x90`-byte live record, and applies the selected song, course,
Ura flag, and player configuration through the existing Enso-facing managers.
The host adapter therefore calls this function instead of reproducing its
many downstream field writes.

The adapter is guarded by the normal class vtable, host-owned phase, valid
manager/vector bounds, an exact live music-ID match, and availability of the
requested difficulty. On success it releases input and hides the opaque host
screen, allowing the original state-11 preload/transition path to continue.
On failure it clears the request and remains in the host browser.

## Live validation

`build-linux/taiko-song-launch-02.log` validates the first complete native
Linux launch. The host browser selected music ID `evedrm` at Oni difficulty,
resolved it to live manager index 7, invoked the native transaction from scene
state 7, and advanced to the stock post-confirm state 11:

```text
[taiko_frontend] native Song Select commit scene=43251D80
    manager=0FEFEDA8 state=7 live_index=7 record=43D79530
    id=evedrm difficulty=3
[fs] open '/data/fumen/evedrm/solo/evedrm_m.bin'
[fs] open '/data/sound/bgm/nsh/SONG_EVEDRM.nsh'
[taiko_atrac] ... SONG_EVEDRM.nub
```

The title then loaded the normal Enso assets and decoded the selected audio.
This proves the host selection controls the actual fumen and song rather than
only dismissing the overlay.

The preceding negative run is also useful: the first resolver treated the
record header as the string object, rejected the launch at state 7, and left
the browser fully usable. A live debugger read established the four-byte
header and the 853-record vector, after which the corrected adapter launched
without relaxing its guards.

Repeatability through Results and back to the host browser is not implemented
yet. Do not mark roadmap macrotask 3 complete until that loop and multiple
song/difficulty combinations are verified.
