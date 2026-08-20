# Standalone USRDIR release

## Build and install

Build the pinned SDL_GPU and minimal MinGW FFmpeg dependencies once, configure,
then build the release artifact:

```sh
scripts/setup_sdl_gpu_mingw.sh
scripts/build_ffmpeg_mingw.sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=mingw-w64.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --target taiko_release
```

`TAIKO_EMBED_PPU_IMAGE` is enabled by default. During the build,
`tools/embed_ppu_image.py` reads `game/EBOOT.elf`, retains its exact two
loadable process-image ranges plus entry/TLS/OPD metadata, and embeds the result
as a Windows resource. To make an old developer-only build instead, configure
with `-DTAIKO_EMBED_PPU_IMAGE=OFF`; that executable still requires an ELF path
argument.

Copy `build/TaikoRecomp.exe`, `build/dxcompiler.dll`, and `build/dxil.dll` into
the user's dumped game directory:

```text
USRDIR/
  TaikoRecomp.exe
  dxcompiler.dll
  dxil.dll
  EBOOT.BIN
  data/
  cache/
  hash/
  install/
  logs/
  updates/
  ...the rest of the original dump...
```

Run `TaikoRecomp.exe` without command-line arguments. It automatically:

- uses its own directory as the direct USRDIR VFS root;
- enables the required TOC/no-spill and offline-network defaults;
- decodes ATRAC3plus in-process and sends PCM through the game's lifted SPU
  mixer rather than a separate media player;
- stores cabinet backup SRAM at `USRDIR/usiobackup.bin`.

SDL3, SDL_shadercross, SPIRV-Cross, the MinGW C++ runtime, libgcc, and
winpthreads are linked statically. Dynamic guest shader compilation requires
the two pinned Microsoft DXC runtime DLLs. Native Windows lets SDL_GPU select
D3D12 or Vulkan; `run-taiko.sh` selects Vulkan when developing under Wine.

## Audio and synchronization

The executable statically links a deliberately minimal FFmpeg 8.1.2 build with
only the ATRAC3plus decoder, WAV demuxer, and required utility/resampling code.
FFmpeg produces source-rate stereo PCM; it does not play or schedule audio.
`cellAtracDecode` still fills the title's three-slot decoder ring, and the
lifted bnusCore SPU mixer remains responsible for voice start/stop, cue resets,
gain, authored `smpl` loops, 44.1-to-48 kHz conversion, and final cellAudio
submission. This keeps the rhythm-game timeline under the game's control.

For streamed songs, `cellAtracSetDataAndGetMemSize` receives only a prefix of a
circular compressed-data buffer. The in-process decoder matches that prefix to
the complete RIFF in the user's own `data/sound/bgm/nub` directory and decodes
it before returning. An uncached selection can therefore add preparation
latency, but playback never advances through temporary silence while a helper
catches up.

`TAIKO_AUDIO_DECODE=0` and `TAIKO_AUDIO_SPU=0` remain diagnostic overrides.
No Python process, `ffmpeg.exe`, audio cache files, or FFmpeg DLLs are needed at
runtime.

## FFmpeg redistribution

The dependency script pins the official FFmpeg 8.1.2 source archive and SHA-256
and configures only LGPL components. Static linking still carries LGPL
redistribution obligations, including notices/source availability and a way for
recipients to relink against a modified FFmpeg. Prepare those compliance
artifacts before publishing binaries; a private build needing only one EXE is
not by itself a complete public distribution package.

## Redistribution note

Embedding is a packaging change, not a copyright transformation. The current
resource contains about 16.9 MiB copied from the user's patched EBOOT `PT_LOAD`
ranges, including executable bytes that the title reads as data. Consequently,
`TaikoRecomp.exe` currently contains game-derived copyrighted material even
and no separate patched ELF exists any more -- the security bypass lives in the lifted code.

Use this mode for private builds from a legitimately obtained dump. Do not
describe the current executable as free of game code or assume it is suitable
for public redistribution. A cleaner public-release design would ship no
embedded process image and reconstruct the required data at install/first run
from the user's own `EBOOT.BIN`, or reduce the dependency to a verified set of
redistributable/generated tables.
