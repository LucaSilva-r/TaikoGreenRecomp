#!/usr/bin/env bash
# Pinned static mbedTLS for both build targets.
#
# The arcade services (MUCHA, the game server) are HTTPS, and the title's own
# cellSsl is a lifecycle shell -- host TLS is what makes them reachable. One
# vendored library keeps the Linux and MinGW builds on a single code path, so a
# handshake bug cannot reproduce on only one of them.
#
#   scripts/build_mbedtls.sh          # both targets
#   scripts/build_mbedtls.sh linux    # or one of them
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version=3.6.4
sha256=ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110
deps_dir="${repo_dir}/third_party/mbedtls-source"
archive="${deps_dir}/mbedtls-${version}.tar.bz2"
source_dir="${deps_dir}/mbedtls-${version}"

mkdir -p "${deps_dir}"
if [[ ! -f "${archive}" ]]; then
    curl --fail --location --output "${archive}" \
        "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${version}/mbedtls-${version}.tar.bz2"
fi
printf '%s  %s\n' "${sha256}" "${archive}" | sha256sum --check --status

if [[ ! -d "${source_dir}" ]]; then
    tar -xf "${archive}" -C "${deps_dir}"
fi

build_target() {
    local name="$1"; shift
    local prefix="${repo_dir}/third_party/mbedtls-${name}"
    local build_dir="${deps_dir}/build-${name}"

    rm -rf "${build_dir}"
    cmake -S "${source_dir}" -B "${build_dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${prefix}" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
        -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
        -DENABLE_PROGRAMS=OFF \
        -DENABLE_TESTING=OFF \
        -DMBEDTLS_FATAL_WARNINGS=OFF \
        "$@"
    cmake --build "${build_dir}" -j"${TAIKO_MBEDTLS_JOBS:-8}"
    cmake --install "${build_dir}"
    echo "mbedTLS ${version} -> ${prefix}"
}

targets=("${@:-linux mingw}")
for target in ${targets[@]}; do
    case "${target}" in
        linux) build_target linux ;;
        mingw) build_target mingw \
                   -DCMAKE_TOOLCHAIN_FILE="${repo_dir}/mingw-w64.cmake" ;;
        *) echo "unknown target: ${target}" >&2; exit 1 ;;
    esac
done
