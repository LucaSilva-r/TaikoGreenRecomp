#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ffmpeg_version=8.1.2
ffmpeg_sha256=464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c
deps_dir="${repo_dir}/third_party/ffmpeg-source"
archive="${deps_dir}/ffmpeg-${ffmpeg_version}.tar.xz"
source_dir="${deps_dir}/ffmpeg-${ffmpeg_version}"
prefix="${repo_dir}/third_party/ffmpeg-mingw"

mkdir -p "${deps_dir}"
if [[ ! -f "${archive}" ]]; then
    curl --fail --location --output "${archive}" \
        "https://ffmpeg.org/releases/ffmpeg-${ffmpeg_version}.tar.xz"
fi
printf '%s  %s\n' "${ffmpeg_sha256}" "${archive}" | sha256sum --check --status

if [[ ! -d "${source_dir}" ]]; then
    tar -xf "${archive}" -C "${deps_dir}"
fi

cd "${source_dir}"
if [[ -f Makefile ]]; then
    make distclean
fi

./configure \
    --prefix="${prefix}" \
    --target-os=mingw32 \
    --arch=x86_64 \
    --cross-prefix=x86_64-w64-mingw32- \
    --pkg-config=x86_64-w64-mingw32-pkg-config \
    --enable-cross-compile \
    --disable-autodetect \
    --disable-everything \
    --enable-avcodec \
    --enable-avformat \
    --enable-avutil \
    --enable-swresample \
    --disable-avdevice \
    --disable-avfilter \
    --disable-swscale \
    --enable-decoder=atrac3p \
    --enable-demuxer=wav \
    --enable-protocol=file \
    --enable-w32threads \
    --enable-small \
    --disable-programs \
    --disable-doc \
    --disable-debug \
    --disable-shared \
    --enable-static \
    --disable-network \
    --extra-cflags=-ffunction-sections \
    --extra-cflags=-fdata-sections

make -j"${TAIKO_FFMPEG_JOBS:-6}"
make install

printf 'MinGW FFmpeg installed at %s\n' "${prefix}"
