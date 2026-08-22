#!/usr/bin/env bash
# Pinned static FreeType for both build targets.
#
# The overlay draws the pairing code in the game's own font, and FreeType's
# rasterizer is what turns it into pixels. Vendored rather than taken from the
# host so the MinGW target needs no extra system package, and so both builds
# rasterize identically.
#
#   scripts/build_freetype.sh          # both targets
#   scripts/build_freetype.sh linux    # or one of them
#   scripts/build_freetype.sh rpi-arm64
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version=2.13.3
sha256=0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
deps_dir="${repo_dir}/third_party/freetype-source"
archive="${deps_dir}/freetype-${version}.tar.xz"
source_dir="${deps_dir}/freetype-${version}"

mkdir -p "${deps_dir}"
if [[ ! -f "${archive}" ]]; then
    curl --fail --location --output "${archive}" \
        "https://downloads.sourceforge.net/project/freetype/freetype2/${version}/freetype-${version}.tar.xz"
fi
printf '%s  %s\n' "${sha256}" "${archive}" | sha256sum --check --status

if [[ ! -d "${source_dir}" ]]; then
    tar -xf "${archive}" -C "${deps_dir}"
fi

build_target() {
    local name="$1"; shift
    local prefix="${repo_dir}/third_party/freetype-${name}"
    local build_dir="${deps_dir}/build-${name}"

    rm -rf "${build_dir}"
    # No compression, no PNG, no shaping: glyph outlines are all the overlay
    # needs, and every one of those would be another cross-built dependency.
    cmake -S "${source_dir}" -B "${build_dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${prefix}" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON \
        "$@"
    cmake --build "${build_dir}" -j"${TAIKO_FREETYPE_JOBS:-8}"
    cmake --install "${build_dir}"
    printf '%s\n' "${version}" > "${prefix}/.taikorecomp-version"
    echo "FreeType ${version} -> ${prefix}"
}

targets=("${@:-linux mingw}")
for target in ${targets[@]}; do
    case "${target}" in
        linux) build_target linux ;;
        rpi-arm64) build_target rpi-arm64 \
                   -DCMAKE_TOOLCHAIN_FILE="${repo_dir}/cmake/raspberry-pi-aarch64.cmake" ;;
        mingw) build_target mingw \
                   -DCMAKE_TOOLCHAIN_FILE="${repo_dir}/mingw-w64.cmake" ;;
        *) echo "unknown target: ${target}" >&2; exit 1 ;;
    esac
done
