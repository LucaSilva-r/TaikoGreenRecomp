# Guest game state and the scene id map

A stable way to know which screen the guest is on, read out of the guest
itself instead of inferred from the files it opens. Addresses are S111 green
(`game/EBOOT.elf`); the mechanism is the same on every EBOOT that shipped with
RTTI (blue, green, red, white, yellow).

## Where transitions happen

Every screen is a `game::sequence::ISequenceTask` subclass. A scene ends by
constructing its successor and handing it to `SequenceController`'s third
virtual, which appends it to the task array at `controller+8`:

```
new NextScene(controller)                  ; ctor stores the controller
controller->vt[2](controller, scene, 0)    ; func_008DA500
```

`func_008DA500` is just `arr = *(ctrl + 8); if (arr.count < arr.cap)
arr.data[arr.count++] = task;`. Every transition goes through it — boot,
attract and test mode included — so it is the single place to watch.

## Naming the scene

The EBOOT kept C++ RTTI, so a task pointer names its own class:

```
task -> vptr -> *(vptr - 4) = typeinfo -> *(typeinfo + 4) = mangled name
```

Scene names are all `N4game<len><Name>E`, e.g. `N4game19GameGhostSongSelectE`.
Anything nested deeper (`game::animation::*`, `game::enso::*`) is not a scene.

Locating the push slot without hardcoding an address, given only the typeinfo
name string:

```
"N4game18SequenceControllerE"   unique string, read-only segments
  -> typeinfo  = &name_ref - 4  unique word, writable segments
  -> vtable    = &ti_ref   - 4  unique word, writable segments
  -> push slot = vtable + 8 + 4*2
```

Single hit at every step on all five RTTI EBOOTs. Green resolves to typeinfo
`0x00F9AE88`, vtable `0x00F9AE68`, slot `0x00F9AE78`, `func_008DA500`.

## Scene object layout

```
+0x00  vptr        1..3 of them: 3 for multiply-inherited scenes such as
+0x04  vptr        GameGhostSongSelect, 1 for GameSongSetup
+0x08  vptr
+0x0C  controller  SequenceController*   (immediately after the last vptr)
+0x10  state       the scene's own state-machine word
+0x14  next        transition selector
```

Virtual slot 4 is `Proc_Main` — `func_001F5CA8` for `GameSongSelect`,
`func_000D2208` for `GameGhostSongSelect`, `func_001EF214` for `GameEntry`.

## Class -> screen

| RTTI class | screen |
|---|---|
| `GameStartup` | boot |
| `GameAttract`, `GameAttractCamera`, `GameAttractDemoPlay`, `GameAttractPlayer` | attract |
| `GameEntry`, `GameMode`, `CollaboSmart` | entry / mode select |
| `GameSongSetup` | song-list build, shared by every flow — names no screen |
| `GameSongSelect` | song select |
| `GameDojoSelect` | dan-i select |
| `GameWaiwaiSongSelect` | waiwai select |
| `GameGhostSongSelect` / `GameBattleSongSelect` | song select, alternative mode |
| `GameEnso` | gameplay |
| `GameGhostEnso` / `GameBattleEnso` | gameplay, alternative mode |
| `GameEnsoResult` | result |
| `GameGhostEnsoResult` / `GameBattleEnsoResult` | result, alternative mode |
| `GameEnsoResultDojo` | dan-i result |
| `GameTotalResult` | total result (red, white, yellow) |
| `GameWaiwaiResult` | waiwai result |
| `GameTutorial`, `GameTutorialTraining`, `GameTutorialWaiwai` | tutorial |
| `GameTokkunMode`, `GameTokkunModeCaller`, `GameTokkunModeResult` | training mode |
| `GameGhostTutorial` | tutorial, alternative mode |
| `GameGhostUserSetting`, `GameGhostEnsoSetting`, `GameBattleSetting`, `GameBattleIntro` | alternative-mode setup: no screen of its own, but the earliest point the mode is known |
| `GameGhostRankUp`, `GameGhostRemind`, `GameGhostReward` | reward, alternative mode |
| `GameRewardShop`, `RewardGasha` | shop / reward |
| `TestMode` | operator service menu |

Green carries the `GameGhost*` family (AI battle), blue the `GameBattle*` one
(the RPG mode); red, white and yellow have neither.

Two screens have no `ISequenceTask` of their own:

- **waitinput** — a sub-state inside `GameAttract`; its own state word at
  scene`+0x10` separates it.
- **intermission** — `game::service::IntermissionWork` /
  `IntermissionController`, a service task on a different list.

## GameEntry's next-scene id

`GameEntry` keeps the mode the player picked as an id at `this+0x10FC`
(written inline at `0x001ED1B4`) and dispatches it through the jump table at
`0x001EF7F8`, 11 entries, dispatch at `0x001EF7D4`:

| id | scene pushed |
|----|--------------|
| 0  | `GameOver` |
| 1  | `GameSongSetup` (normal enso) |
| 2, 5-9 | nothing |
| 3  | `GameRewardShopSequencer<GameSongSetup>` / `GameSongSetup` |
| 4  | `CollaboSmart` |
| 10 | `GameGhostUserSetting` — AI battle |

So AI battle runs `GameEntry(id=10)` -> `GameGhostUserSetting` ->
`GameGhostTutorial` -> `GameSongSetup` -> `GameGhostSongSelect`: the mode is
settled two scenes before anything in the ghost tree is touched.

The id only enumerates what `GameEntry` itself pushes — startup, attract, test
mode and the result screens never appear in it — so it is a mode signal, not a
state source. The push hook above is the state source.

The Lumen native `SetNextScene(id)` (unique row, resolvable by name) carries
the same id from the entry scene's script.
