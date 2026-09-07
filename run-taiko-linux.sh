#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
sdl_prefix="${TAIKO_SDL_GPU_ROOT:-${repo_dir}/third_party/sdl-gpu-linux}"
dxc_lib="${sdl_prefix}/dxc-v1.8.2502/lib"

if [[ ! -x "${repo_dir}/build-linux/taiko_boot" ]]; then
    printf 'Native SDL build is missing. Configure and build it with:\n' >&2
    printf '  cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \\\n' >&2
    printf '    -DTAIKO_RSX_BACKEND=sdl_gpu -DTAIKO_INPUT_BACKEND=sdl3 \\\n' >&2
    printf '    -DTAIKO_AUDIO_BACKEND=sdl3 -DTAIKO_INPROCESS_ATRAC=ON \\\n' >&2
    printf '    -DTAIKO_EMBED_PPU_IMAGE=OFF\n' >&2
    printf '  cmake --build build-linux\n' >&2
    exit 2
fi

export LD_LIBRARY_PATH="${dxc_lib}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

command=("${repo_dir}/build-linux/taiko_boot"
    "${repo_dir}/game/EBOOT.elf" "$@")

cd "${repo_dir}"
if [[ "${TAIKO_CONSOLE_LOG:-0}" == "1" ]]; then
    exec "${command[@]}"
fi

log_file="${TAIKO_LOG_FILE:-${repo_dir}/build-linux/taiko.log}"
printf 'Taiko log: %s\n' "${log_file}"
exec "${command[@]}" >"${log_file}" 2>&1
