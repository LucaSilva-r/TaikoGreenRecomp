#!/usr/bin/env bash
# Runs inside the Debian Bookworm cross-build container. The default produces
# the playable SDL_GPU/SDL audio build; TAIKO_RPI_HEADLESS=1 retains the small
# null-backend diagnostic build used for the first CPU/runtime milestone.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${TAIKO_DEP_JOBS:-4}"
export TAIKO_MBEDTLS_JOBS="${TAIKO_MBEDTLS_JOBS:-${jobs}}"
export TAIKO_FREETYPE_JOBS="${TAIKO_FREETYPE_JOBS:-${jobs}}"
export TAIKO_FFMPEG_JOBS="${TAIKO_FFMPEG_JOBS:-${jobs}}"

mbedtls_stamp="${repo_dir}/third_party/mbedtls-rpi-arm64/.taikorecomp-version"
mbedtls_lib="${repo_dir}/third_party/mbedtls-rpi-arm64/lib/libmbedtls.a"
if [[ ! -f "${mbedtls_lib}" ]] || [[ ! -f "${mbedtls_stamp}" ]] ||
   [[ "$(<"${mbedtls_stamp}")" != 3.6.4 ]]; then
    "${repo_dir}/scripts/build_mbedtls.sh" rpi-arm64
fi
freetype_stamp="${repo_dir}/third_party/freetype-rpi-arm64/.taikorecomp-version"
freetype_lib="${repo_dir}/third_party/freetype-rpi-arm64/lib/libfreetype.a"
if [[ ! -f "${freetype_lib}" ]] || [[ ! -f "${freetype_stamp}" ]] ||
   [[ "$(<"${freetype_stamp}")" != 2.13.3 ]]; then
    "${repo_dir}/scripts/build_freetype.sh" rpi-arm64
fi

headless="${TAIKO_RPI_HEADLESS:-0}"
if [[ "${headless}" == 1 ]]; then
    backend_args=(
        -DTAIKO_HEADLESS=ON
        -DTAIKO_INPROCESS_ATRAC=OFF
    )
else
    sdl_stamp="${repo_dir}/third_party/sdl-gpu-rpi-arm64/versions.txt"
    if [[ ! -f "${sdl_stamp}" ]]; then
        "${repo_dir}/scripts/setup_sdl_gpu_rpi_arm64.sh"
    fi
    ffmpeg_stamp="${repo_dir}/third_party/ffmpeg-rpi-arm64/.taikorecomp-version"
    ffmpeg_lib="${repo_dir}/third_party/ffmpeg-rpi-arm64/lib/libavcodec.a"
    if [[ ! -f "${ffmpeg_lib}" ]] || [[ ! -f "${ffmpeg_stamp}" ]] ||
       [[ "$(<"${ffmpeg_stamp}")" != 8.1.2 ]]; then
        "${repo_dir}/scripts/build_ffmpeg_rpi_arm64.sh"
    fi
    backend_args=(
        -DTAIKO_HEADLESS=OFF
        -DTAIKO_RSX_BACKEND=sdl_gpu
        -DTAIKO_INPUT_BACKEND=sdl3
        -DTAIKO_AUDIO_BACKEND=sdl3
        -DTAIKO_INPROCESS_ATRAC=ON
        -DTAIKO_SDL_GPU_ROOT="${repo_dir}/third_party/sdl-gpu-rpi-arm64"
        -DTAIKO_FFMPEG_ROOT="${repo_dir}/third_party/ffmpeg-rpi-arm64"
    )
fi

cmake -S "${repo_dir}" -B "${repo_dir}/build-rpi-arm64" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${repo_dir}/cmake/raspberry-pi-aarch64.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTAIKO_EMBED_PPU_IMAGE=OFF \
    -DTAIKO_OVERLAY_FONT_FILE="${repo_dir}/fonts/font.ttf" \
    -DTAIKO_COMPILE_JOBS="${TAIKO_COMPILE_JOBS:-4}" \
    -DBUILD_TESTING=OFF \
    "${backend_args[@]}"
build_targets=(taiko_boot)
if [[ "${TAIKO_RPI_BUILD_REPLAY:-0}" == 1 ]]; then
    build_targets+=(rsx_replay)
fi
cmake --build "${repo_dir}/build-rpi-arm64" --target "${build_targets[@]}" \
    --parallel "${jobs}"

binary="${repo_dir}/build-rpi-arm64/taiko_boot"
if ! aarch64-linux-gnu-readelf -h "${binary}" | grep -q 'Machine:.*AArch64'; then
    echo "Cross-build did not produce an AArch64 executable: ${binary}" >&2
    exit 1
fi
aarch64-linux-gnu-readelf -h "${binary}" |
    sed -n '/Class:/p; /Data:/p; /Machine:/p'

if [[ "${headless}" != 1 ]]; then
    bundle="${repo_dir}/build-rpi-arm64/bundle"
    cmake -E remove_directory "${bundle}"
    cmake -E make_directory "${bundle}/lib"
    cmake -E copy "${binary}" "${bundle}/taiko_boot"
    cp -a "${repo_dir}/third_party/sdl-gpu-rpi-arm64/dxc-v1.8.2502/lib/libdxcompiler.so"* \
        "${bundle}/lib/"
    cp -a "${repo_dir}/third_party/sdl-gpu-rpi-arm64/dxc-v1.8.2502/lib/libdxil.so"* \
        "${bundle}/lib/"
    printf 'Playable bundle: %s\n' "${bundle}"
fi
