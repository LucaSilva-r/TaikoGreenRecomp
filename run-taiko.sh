#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

if [[ ! -f "${repo_dir}/build/dxcompiler.dll" ]]; then
    printf 'SDL_GPU Windows runtime is missing. Run:\n' >&2
    printf '  scripts/setup_sdl_gpu_mingw.sh\n' >&2
    printf '  cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release\n' >&2
    printf '  cmake --build build\n' >&2
    exit 2
fi

export TAIKO_AUDIO_OFFSET_MS=60
export PS3_VFS_ROOT="${PS3_VFS_ROOT:-${repo_dir}/game/vfs}"
export PS3_TOC_SET="${PS3_TOC_SET:-0x1027c58,0x1037a88,0x1047a38}"
export FLOW_NOSPILL="${FLOW_NOSPILL:-1}"
export TAIKO_DNS_LOOPBACK="${TAIKO_DNS_LOOPBACK:-1}"
export TAIKO_OFFLINE_COMPLETE="${TAIKO_OFFLINE_COMPLETE:-1}"
export TAIKO_FS_YIELD="${TAIKO_FS_YIELD:-0}"
# The decoded ATRAC path and the real bnusCore SPU mixer are the normal audio
# configuration. Keep explicit =0 overrides for silent/headless diagnostics.
export TAIKO_AUDIO_DECODE="${TAIKO_AUDIO_DECODE:-1}"
export TAIKO_AUDIO_SPU="${TAIKO_AUDIO_SPU:-1}"
# VP_LEQUAL_LESS (strict depth) traded missing text for missing backdrops --
# the real issue is elsewhere (UI composited via offscreen RTs). Leave the
# spec-correct LEQUAL default; export VP_LEQUAL_LESS=1 manually to compare.
# RTT_UNREVERSE is retained in the backend as an opt-in diagnostic only.  The
# normal path must preserve RSX FIFO order, as RPCS3 does; replay-time reversal
# loses the scene hierarchy and is not a valid layer mechanism.
# RTT_SORT_LUMEN_GROUPS is a diagnostic only.  A source-Z value is local to
# its nested Lumen movie, so globally sorting these groups crosses unrelated
# scene branches (Song Select loses its header/footer/backdrop ordering).
# Leave it unset during normal runs.
# Render-target saves and RSX 2D probes are opt-in diagnostics.  Do not set
# RTT_SAVERT, RTT_SAVEA, or GCM2D_PROBE in the normal launcher.
export WINEDEBUG="${WINEDEBUG:--all}"
# Native Windows lets SDL_GPU choose between D3D12 and Vulkan. Under Wine,
# prefer its Vulkan path directly and avoid an unnecessary D3D12 translation.
export TAIKO_GPU_DRIVER="${TAIKO_GPU_DRIVER:-vulkan}"

command=(wine "${repo_dir}/build/taiko_boot.exe"
    "${repo_dir}/game/EBOOT.elf" "$@")

cd "${repo_dir}"

if [[ "${TAIKO_CONSOLE_LOG:-0}" == "1" ]]; then
    exec "${command[@]}"
fi

log_file="${TAIKO_LOG_FILE:-${repo_dir}/build/taiko.log}"
printf 'Taiko log: %s\n' "${log_file}"
exec "${command[@]}" >"${log_file}" 2>&1
