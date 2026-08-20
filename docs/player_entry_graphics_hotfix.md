# Player Entry graphics and offline title hotfix

The Player Entry repair has two different layers. They should not be conflated
when investigating later Lumen screens.

## General renderer fixes

The RSX backend fixes (made in the since-removed D3D12 backend, and carried
forward into `rsx_sdl_gpu_backend.c`) are general:

- deswizzle non-linear A8R8G8B8 and R8 textures;
- avoid the obsolete SRV heap-slot collision that made the timer alternate
  between black and its number;
- preserve each Lumen compositing group while ordering whole groups, fixing
  transparency and layer order without splitting a group around its backdrop.

These apply to all content using the affected texture formats and Lumen draw
pattern. They are renderer behavior, not Player Entry asset exceptions.

## Offline scene completion

Without an arcade server, Green starts its initial-data request but never gets
the callback. `OnlineCheck` therefore remained in state 0 indefinitely. The
real callback has a short failure path which:

1. changes `OnlineCheck::state` to 3;
2. clears the callback result flags at offsets `+4` through `+8`;
3. raises bit `0x10000000` in the network-status flags object.

`settle_offline_network_state()` in `src/taiko_usio.cpp` reproduces only that
failure branch. It is enabled by `TAIKO_OFFLINE_COMPLETE`, which defaults to 1
in `run-taiko.sh`. Set `TAIKO_OFFLINE_COMPLETE=0` to disable both this completion
and the title compatibility hook below.

## Asset-specific title release

After correct offline completion, the Player Entry scene selects its expected
coin-only branch, but one nested clip remains on its authored frame-0 `Stop`:

- Lumen character/sprite ID: `817` (`0x331`);
- frame count: 26;
- texture at the visible frame: `entry/img00320.nut`;
- expected settled frame: 20;
- RPCS3 reference state: current frame 20, stopped, visible;
- broken recomp state: current frame 0, stopped, hidden.

`src/taiko_lumen.cpp` overrides the indirect `lumen::Sprite::AdvanceFrame`
dispatch at guest address `0x003DF910`. It releases a clip only when all of the
following are true:

- `TAIKO_OFFLINE_COMPLETE` is enabled;
- the object uses the `lumen::Sprite` vtable `0x00F09030`;
- character ID is exactly 817;
- current frame is 0 and total frame count is exactly 26;
- its stop flag is set;
- `OnlineCheck` is in failure state 3;
- the real callback's unavailable bit `0x10000000` is set.

The hook runs once per process, sets only visibility and the stop flag, then
calls the original `Sprite::AdvanceFrame`. The original Lumen timeline performs
the complete animation and stops naturally at frame 20. A live GDB experiment
confirmed that this produces the intact Japanese Player Entry title before the
hook was made permanent.

This is deliberately a compatibility hotfix, not a general policy for stopped
Lumen clips. If another animation is missing, do not broaden these predicates.
First compare its live frame, stop, visibility, and parent-scene state against
RPCS3 and find its own missing transition.

The expected diagnostic lines are:

```text
[taiko_lumen] installed player-entry title Sprite::AdvanceFrame hook
[taiko_netstate] completed unavailable initial-data check: state 0 -> 3, unavailable bit set
[taiko_lumen] released offline player-entry title sprite object=........
```

The online BanaPassport prompts remain absent by design while the recomp has no
arcade-network implementation. The English `Player Entry` reference texture is
also a modded asset; the unmodified dump displays its Japanese equivalent.
