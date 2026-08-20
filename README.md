# TaikoRecomp

Static recompilation of **Taiko no Tatsujin** (Bandai Namco, System 357 arcade,
`SCEEXE001` / S111 "Green") to native Windows and Linux executables, using
[ps3recomp](https://github.com/sp00nznet/ps3recomp).

The graphical host is SDL3: SDL_GPU rendering plus SDL3 input, with headless
null backends for tests. Windows can be built from Linux via mingw-w64 + Wine.
No game binaries or data are in this repo -- bring your own dump. Nothing
derived from a dump is tracked either: the lifted sources, the address and
symbol tables, and the dump itself are all produced locally on your machine.
See [NOTICE.md](NOTICE.md) for the full statement and for the vendored
ps3recomp attribution.

## Status

Boots through its C++ and arcade-service initialization, brings up SDL_GPU,
loads real game data, and reaches gameplay. Taiko submits RSX clears, textured
quads, indexed models, and offscreen render-target chains; the portable RSX
recorder and SDL_GPU renderer reproduce them on Linux and Windows. With the Green
dongle/VU behavior ported from Taiko Zucchini, the live screen reports Security
Checks 1-3, USB Dongle, USB Memory, and cabinet I/O all `OK`. The old
zero-draw/black-window renderer stall is fixed, and the game now polls a native
virtual PS3A-USJ board for cabinet switches, drum sensors, and backup SRAM.

```text
SECURITY CHECK 1       OK
SECURITY CHECK 2       OK
SECURITY CHECK 3       OK
USB DONGLE             OK
USB MEMORY             OK
USB CAMERA             NG
USB CAMERA SERVICE     NG
I/O                    OK
CARD R/W               -
```

```
system::GameContent::GameContent() cellGameGetParamString() CELL_GAME_RET_OK
[fs] open '/dev_hdd0/game/SCEEXE001/USRDIR/data/config/common/config.xml' -> fd 3
[fs] read fd=3 -> 691 (magic=3C3F786D)                 <- "<?xm"
[fs] open '/dev_hdd0/game/SCEEXE001/USRDIR/data/sound/config/config.bin' -> fd 3
[fs] read fd=3 -> 2640 (magic=6E757363)                <- "nusc"
[fs] open '/dev_hdd0/game/SCEEXE001/USRDIR/data/font/dfhsr4.nut' -> fd 3
[fs] read fd=3 -> 4096 (magic=4E545033)                <- "NTP3"
[D3D12] Initialization complete (1280x720, 2 buffers, pipeline=ready)
[cellAudio] PortOpen(nChannel=8, nBlock=8)
[SYS] sys_ppu_thread_create name="MuchaMainThread" ...
[SYS] sys_ppu_thread_create name="usj_usio_worker_thread" ...
[D3D12] bind_texture(unit=0, offset=0xCC0300, fmt=0x87, 512x512)
[RSX] DRAW_ARRAYS prim=8 first=0 count=4
[VP] pipeline ready (10 instrs)
[FP] guest FP pipeline ready (fp=0x00CC0141)
[D3D12] draw_arrays #9000 prim=8 first=0 count=4
```

Run it with the flags that matter:

```sh
PS3_VFS_ROOT=game/vfs \
PS3_TOC_SET=0x1027c58,0x1037a88,0x1047a38 \
FLOW_NOSPILL=1 \
TAIKO_DNS_LOOPBACK=1 \
wine build/taiko_boot.exe game/EBOOT.recomp.elf
```

`FLOW_NOSPILL=1` is **required**, not a debug option -- see *The TOC spill* below.

### Standalone USRDIR release

The release build embeds the PPU process image and supplies the required runtime
defaults itself. Build the named release artifact with:

```sh
scripts/build_ffmpeg_mingw.sh
cmake --build build --target taiko_release
```

Copy `build/TaikoRecomp.exe` into the dumped game's `USRDIR` directory, beside
`EBOOT.BIN`, `data`, `cache`, `hash`, `install`, `logs`, and `updates`, then run
it without arguments. The executable directory becomes the VFS root directly;
no `game/vfs` tree, symlinks, `PS3_VFS_ROOT`, or separate
`EBOOT.recomp.elf`, Python helper, FFmpeg executable/DLL, or decoded-audio cache
is needed. Cabinet backup SRAM is written as `USRDIR/usiobackup.bin`.

The development invocation with an explicit ELF argument remains supported and
continues to use the legacy VFS layout.

See [docs/release_build.md](docs/release_build.md) for the pinned in-process
ATRAC dependency and the important redistribution notes about FFmpeg and
embedded game bytes.

## Controls

The virtual USIO board consumes keyboard and XInput directly. Drum hits are
edge-triggered one-frame sensor pulses; holding a key does not auto-repeat.

| Cabinet input | Keyboard | XInput |
|---|---|---|
| Left rim | `D` | D-pad left/up, LB, LT |
| Left center | `F` | D-pad right/down |
| Right center | `J` | X or A |
| Right rim | `K` | Y, B, RB, or RT |
| Enter | `Enter` | Start |
| Menu up/down | Arrow up/down | D-pad up/down |
| Coin | `C` | Left-stick click |
| Test | `F1` | Back |
| Service | `F2` | Right-stick click |

The second XInput controller feeds player 2's drum. The keyboard mappings feed
player 1. Test is a toggle, matching the cabinet's latched operator switch.

| Metric | Value |
|---|---|
| Binary | `EBOOT.elf`, 17.7 MB, ELF64 big-endian PPC64, `ET_EXEC` |
| Entry | OPD `0xfa4a68` -> code `0x10240`, TOC `0x1027c58` (of three) |
| Functions detected | 32,650 (30,073 `.opd` + 2,150 Ghidra-only starts) |
| Functions lifted | 43,422 (incl. 12,232 tail-entry wrappers) |
| Unlifted instructions | **0** |
| Unresolved indirect calls at runtime | **0** |
| Imports | 326 NIDs / 23 libraries |
| Named functions | 1,585 from Ghidra |
| Executable | `taiko_boot.exe` / release copy `TaikoRecomp.exe`, ~121 MB |

## The TOC spill

`ps3_indirect_call()` in `ppu_loader.cpp` unconditionally did
`vm_write64(r1 + 40, r2)` -- the ELFv1 glink TOC save. A lifted **tail-entry
wrapper runs on its caller's frame**, so `r1` is frequently still the caller's,
and the store lands on the *caller's* reserved TOC doubleword. The caller's
later `ld r2, 0x28(r1)` then restores a foreign TOC.

That is the whole early-boot failure. With a wrong-but-plausible r2, every
TOC-relative load returns a valid-looking wrong pointer: `lwz r31, -0x7F00(r2)`
produced a `this` pointer that was actually an `.opd` address, whose vtable
dispatch read `code=0` / `toc=0`; `_Printf` (`func_0059B898`) loaded its
output-callback descriptor from `lwz r3, -0x743C(r2)` and got `0x400440`, the
epilogue of an lv2 syscall wrapper, which it then called as an OPD.

`ppu_hle.cpp` already gated its copy of this store behind `FLOW_NOSPILL`, with
a comment describing exactly this ("in the frameless-cascade it can clobber a
caller frame slot"). The copy in `ppu_loader.cpp` was not gated. It is now, and
setting `FLOW_NOSPILL=1` takes the boot from 48 log lines to 5,028.

Diagnostic that found it: `PS3_TOC_SET` (also added to `ps3_indirect_call`) --
list every TOC the image declares and the first r2 outside the set is logged
with a guest backtrace. Essential on a multi-TOC image, where corruption
surfaces thousands of instructions from its cause.

## What unlocked rendering

Two runtime bugs were corrupting execution before Taiko could submit a frame:

- `cellAudioPortOpen()` placed its guest PCM arena at `0x01000000`. Taiko's
  loaded `.opd` extends through `0x0101FC58`, so opening the first audio port
  zeroed valid function descriptors, including the USIO worker callback. Audio
  ports now use eight fixed slots beginning at `0x50000000`, beyond the normal
  `sys_memory` arena.
- The lifted code and runtime declared their continuation pointer with
  `__declspec(thread)`. MinGW warned that it ignored that attribute, making the
  pointer process-global: concurrent guest threads could steal one another's
  pending lifted continuation. It now uses portable C++ `thread_local` in both
  the lifter output and runtime.

The network shim also has to return a real big-endian PS3 `hostent`, containing
guest EAs rather than native host pointers. `TAIKO_DNS_LOOPBACK=1` supplies that
structure for the cabinet hostnames until the ALL.Net transport exists.

Once draws arrived, two RSX backend omissions kept their pixels black:

- `NV4097_SET_VERTEX_DATA_ARRAY_OFFSET` stores the memory location in bit 31.
  Passing the encoded value to the generic offset resolver fetched zeros from
  the wrong address space. The VP path now separates the location bit from the
  31-bit offset; Taiko's quad positions and texture coordinates decode correctly.
- The 512x512 font atlas is `CELL_GCM_TEXTURE_COMPRESSED_DXT23` (`0x87`), not
  the previously supported B8/A8R8G8B8 formats. Unsupported textures became a
  null SRV, so the real fragment shader sampled black. DXT23 is now uploaded as
  native D3D12 `BC2_UNORM`, producing the actual security-check glyphs.

## Why this target is unusual

- **No decryption needed.** Arcade EBOOTs are debug fSELFs (`key_revision
  0x8000`) with plaintext segments. `ps3recomp/tools/unfself.py` rebuilds the
  ELF directly -- no keys, no `scetool`.
- **The anti-tamper is easier here than under emulation.** Dongle check,
  `s357security.bin`, the `hash/*.md5` manifests and MUCHA auth are all just
  functions that can be made to return success. Nothing to time-attack.
- **The arcade I/O is the interesting part.** Drums, coin/credit and SRAM live
  on a USIO board behind `cellUsbd`. See the sibling `ITAIKO-Firmware` and
  `namco357-dongle` repos.

## Build

Build the pinned dependencies once. They install into `third_party/`, which
is separate from `build*/` so a build directory stays disposable:

```sh
scripts/setup_sdl_gpu_mingw.sh    # SDL3 + SDL_shadercross + DXC (Windows target)
scripts/setup_sdl_gpu_linux.sh    # the same, for the native Linux build
scripts/build_ffmpeg_mingw.sh     # minimal static ATRAC3plus decoder
```

Then lift and build:

```sh
sudo dnf install mingw64-gcc-c++          # Fedora; adjust per distro
python3 ps3recomp/tools/unfself.py "<game>/USRDIR/EBOOT_ORIGINAL.BIN" -o game/EBOOT.elf
python3 tools/patch_taiko_security.py game/EBOOT.elf game/EBOOT.recomp.elf
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
mkdir -p game/vfs/game && ln -s "<game dir>" game/vfs/game/SCEEXE001
PS3_VFS_ROOT=game/vfs PS3_TOC_SET=0x1027c58,0x1037a88,0x1047a38 FLOW_NOSPILL=1 TAIKO_DNS_LOOPBACK=1 \\
    wine build/taiko_boot.exe game/EBOOT.recomp.elf
```

Regenerating the lifted source (`src/recomp/`, gitignored):

```sh
python3 ps3recomp/tools/find_functions.py game/EBOOT.elf --call-graph -o game/functions.raw.json
# then clip to code_end -- see config.toml [lift]
python3 ps3recomp/tools/ppu_lifter.py game/EBOOT.recomp.elf \
    --functions game/functions.json --code-end 0xa1f890 \
    --hle-stubs meta/EBOOT.imports.json \
    --extra-targets meta/jt_seeds.json -o src/recomp -j $(nproc)
```

Both flags matter. Without `--code-end` the boundary detector promotes 1,441
rodata blobs into "functions" spanning 2.9 MB. Without `--hle-stubs` the 326
import trampolines in `.sceStub.text` are never lifted, so every firmware call
dies as an unresolved reference.

## Recovered jump tables

`tools/find_toc_jumptables.py` finds a switch shape `ppu_lifter.py` does not:

```
lwz    rB, off(r2)      ; table base, via the TOC
rlwinm rI, rX, 2, ...   ; index * 4
lwzx   rT, rI, rB       ; entry is a SIGNED OFFSET FROM THE BASE
add    rT, rT, rB       ; target = base + entry
mtctr  rT ; bctr
.word  ...              ; the table, inline right after the bctr
```

587 tables, 5,423 case targets. Every one lands at an address that was never
lifted, so the first switch the game hits dies as "unresolved indirect call".
The entries are what earlier looked like harmless `TODO: .word` data.

The base is taken as `bctr+4` rather than dereferenced from the TOC: this game
runs **at least three TOCs** (`0x1027c58`, `0x1037a88`, `0x1047a38` all observed
live in r2), so there is no single r2 to resolve against, and guessing wrong
resolves silently to garbage.

Feed the result to the *lifter*, not to `find_functions`:

```sh
python3 tools/find_toc_jumptables.py game/EBOOT.elf --code-end 0xa1f890 -o meta/jt_seeds.json
# ... then ppu_lifter.py --extra-targets meta/jt_seeds.json
```

`--extra-targets` is a flag added to `ppu_lifter.py` here. It unions the
addresses into `branch_targets` so the mid-function pass emits a **tail-entry
wrapper** for each -- lifting from the address to the enclosing function's end,
sharing its stack frame. Seeding them into `find_functions` instead makes each
case a real function, which clips the parent's extent and leaves the case
running an epilogue for a frame it never built. (12,604 wrappers via
`--extra-targets`, against 7,087 without.)

## Blockers

**1. Remaining cabinet peripherals.** The PS3A-USJ board now enumerates and the
screen renders `I/O OK`. USB Camera and Camera Service remain `NG`, while
`CARD R/W` remains `-`; the BanaPassport PN53x command responder and camera are
deliberately outside the first USIO implementation.

Two earlier boot diagnoses were wrong and are recorded here so they are not
redone:

- The `[HOTREAD8] spinning on 0x01405A30` reports are **not** a deadlock. That
  address is `r28+0xE08`, the constant source byte of a `bdnz` memset in the CRT
  allocator; the detector counts consecutive reads of one address. The total is
  a fixed 220 reports (~44 MB of fill, about three 15 MB memsets) and is
  identical at 120 s and at 10 minutes -- the memsets complete.
- Not an allocator infinite loop either. With alignment `r31 = 0` the
  `ble cr4` at `0x9E86E4` exits the retry path every iteration, and the lifted
  CR4 handling is correct (`cmplwi cr4` writes field shift 12, `ble cr4` reads
  `!((cr>>12)&4)`).

**2. cellAtrac: 0 of 17 NIDs implemented.** Not in ps3recomp at all. Every song
is ATRAC3+. Needs a new HLE module over a real decoder (FFmpeg has one). Nothing
about this game matters until audio works.

**3. BanaPassport/card reader.** USIO register `0x0080` exposes an idle reader,
but the PN53x request/response transport at `0x7000`/`0x7400` is not emulated
yet. Taiko Zucchini's `hooks/bpreader_hook.c` is the working reference.

Zucchini normally applies its dongle/VU patches to the live PS3 instruction
stream. That cannot alter code ps3recomp has already translated, so
`tools/patch_taiko_security.py` ports the relevant Green patches to a separate
`EBOOT.recomp.elf` before lifting: forced USB candidates, authenticate-time
stat bypasses, and the caller-aware VID/PID/serial mock. The original ELF stays
untouched, and the Zucchini SPRX loader patch is deliberately not included.

**4. cellSpurs Taskset2 / JobChain APIs missing.** The game creates its own
tasksets and job chains, and ships 5 SPU images. Nothing dispatches them yet.

**5. libSceSmart: 0 of 12.** Namco's own `/data/module/libsmart.sprx`. Also a
plaintext fSELF, so `tools/lift_prx.py` is an option if stubbing fails.

## Upstream fixes carried in `ps3recomp/`

Four build breaks, none Taiko-specific -- worth sending upstream:

- `libs/spurs/spurs_taskset.h` re-declared `vm_read32`/`vm_read64`/`vm_write32`/
  `vm_write64` non-static ahead of `ppu_memory.h`'s `static inline` definitions.
  Now includes the real header instead.
- `libs/video/cellGcmSys.c` declared `Sleep` inline with a bare `__declspec`.
- `libs/video/rsx_commands.c` used `getenv` with no `<stdlib.h>`; the implicit
  declaration then made the `const char*` initializer an int-conversion error.
- `libs/network/sceNpCommerce.c` and `sceNpCommerce2.c` both defined six
  `sceNpCommerce2*` functions. The lifecycle six now live only in
  `sceNpCommerce2.c`, which implements them against real request objects.

## Layout

```
config.toml       per-module HLE/stub decisions, loader + lift constants
CMakeLists.txt    boot harness; links lifted code + ps3recomp runtime
mingw-w64.cmake   Linux -> Windows cross toolchain (Fedora sysroot paths)
LICENSE           MIT
NOTICE.md         what is and is not in this repo; ps3recomp attribution
src/gen/          generated HLE stubs + NID dispatch table
src/taiko_*.c*    title setup, network aliases, and virtual PS3A-USJ cabinet I/O
ps3recomp/        the vendored toolkit (flattened, not a submodule)
tools/ scripts/   lifting, patching, capture and dependency-setup tooling

meta/             loader metadata, import table (doubles as --hle-stubs input)
ghidra_out/       function and symbol maps exported from Ghidra

Generated locally, never tracked:

game/             EBOOT.elf, functions.json, the VFS mounts
src/recomp/       lifted PPU source, 269 MB                    (regenerate)
src/spu_gen/      lifted SPU images
third_party/      installed SDL3/shadercross/DXC/FFmpeg prefixes
build/            MinGW (Windows) output; build-linux/ is the native build
```
