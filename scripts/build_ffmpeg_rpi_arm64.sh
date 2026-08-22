#!/usr/bin/env bash
# Pinned minimal static FFmpeg for the Raspberry Pi ARM64 ATRAC3plus path.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version=8.1.2
sha256=464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c
deps_dir="${repo_dir}/third_party/ffmpeg-source"
archive="${deps_dir}/ffmpeg-${version}.tar.xz"
source_dir="${deps_dir}/ffmpeg-${version}"
build_dir="${deps_dir}/build-rpi-arm64"
prefix="${repo_dir}/third_party/ffmpeg-rpi-arm64"

for tool in curl sha256sum tar make aarch64-linux-gnu-gcc; do
    command -v "${tool}" >/dev/null || {
        printf 'Missing required tool: %s\n' "${tool}" >&2
        exit 2
    }
done

mkdir -p "${deps_dir}"
if [[ ! -f "${archive}" ]]; then
    curl --fail --location --output "${archive}" \
        "https://ffmpeg.org/releases/ffmpeg-${version}.tar.xz"
fi
printf '%s  %s\n' "${sha256}" "${archive}" | sha256sum --check --status

if [[ ! -d "${source_dir}" ]]; then
    tar -xf "${archive}" -C "${deps_dir}"
fi
# Configure out of tree.  A pristine FFmpeg source tarball contains a
# Makefile, but it is not a configured build and `make distclean` there tries
# to include /tests/Makefile because SRC_PATH has not been generated yet.
rm -rf "${build_dir}"
mkdir -p "${build_dir}"

cd "${build_dir}"
"${source_dir}/configure" \
    --prefix="${prefix}" \
    --target-os=linux \
    --arch=aarch64 \
    --cross-prefix=aarch64-linux-gnu- \
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
    --enable-pthreads \
    --enable-small \
    --disable-programs \
    --disable-doc \
    --disable-debug \
    --disable-shared \
    --enable-static \
    --disable-network \
    --extra-cflags=-ffunction-sections \
    --extra-cflags=-fdata-sections

make -j"${TAIKO_FFMPEG_JOBS:-4}"
make install
printf '%s\n' "${version}" >"${prefix}/.taikorecomp-version"
printf 'Raspberry Pi ARM64 FFmpeg installed at %s\n' "${prefix}"
