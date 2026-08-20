#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="${TAIKO_SDL_GPU_ROOT:-${repo_dir}/third_party/sdl-gpu-linux}"
source_dir="${prefix}/.src"
work_dir="${prefix}/.build"
download_dir="${prefix}/.downloads"
jobs="${TAIKO_DEP_JOBS:-4}"

sdl_version="3.4.10"
sdl_url="https://github.com/libsdl-org/SDL/releases/download/release-${sdl_version}/SDL3-${sdl_version}.tar.gz"
sdl_sha256="12b34280415ec8418c864408b93d008a20a6530687ee613d60bfbd20411f2785"
shadercross_commit="e55cf5e31ced6f3d1be5cc6d0c50e99384f9f4ba"
spirv_cross_commit="1a6169566c73d3da552748fc372fe2bbb856e46e"
# The unused vendored toolchain revisions are recorded because they are part
# of the pinned shadercross tree. We deliberately use Microsoft's DXC binary
# instead of building shadercross's LLVM/DXC submodule.
spirv_headers_commit="ad9184e76a66b1001c29db9b0a3e87f646c64de0"
spirv_tools_commit="0539c81f69a3daeb706fd3477dca61435b475156"
dxc_source_commit="2c84a1c5ab7091608c97df6ba5ccf46e71c322eb"
dxc_version="v1.8.2502"
dxc_asset="linux_dxc_2025_02_20.x86_64.tar.gz"
dxc_url="https://github.com/microsoft/DirectXShaderCompiler/releases/download/${dxc_version}/${dxc_asset}"
dxc_sha256="e0580d90dbf6053a783ddd8d5153285f0606e5deaad17a7a6452f03acdf88c71"

for tool in cmake git ninja curl sha256sum tar; do
    command -v "${tool}" >/dev/null || {
        printf 'Missing required tool: %s\n' "${tool}" >&2
        exit 2
    }
done
mkdir -p "${prefix}" "${source_dir}" "${work_dir}" "${download_dir}"

download_verified() {
    local url="$1" output="$2" expected="$3"
    if [[ -f "${output}" ]] && printf '%s  %s\n' "${expected}" "${output}" | sha256sum -c - >/dev/null 2>&1; then
        return
    fi
    local partial="${output}.part"
    curl -L --fail --retry 3 --output "${partial}" "${url}"
    printf '%s  %s\n' "${expected}" "${partial}" | sha256sum -c -
    mv "${partial}" "${output}"
}

checkout_exact() {
    local url="$1" directory="$2" commit="$3"
    if [[ ! -d "${directory}/.git" ]]; then
        git clone --filter=blob:none --no-checkout "${url}" "${directory}"
    fi
    git -C "${directory}" fetch --depth 1 origin "${commit}"
    git -C "${directory}" checkout --detach "${commit}"
    [[ "$(git -C "${directory}" rev-parse HEAD)" == "${commit}" ]]
}

sdl_archive="${download_dir}/SDL3-${sdl_version}.tar.gz"
download_verified "${sdl_url}" "${sdl_archive}" "${sdl_sha256}"
sdl_source="${source_dir}/SDL3-${sdl_version}"
if [[ ! -f "${sdl_source}/CMakeLists.txt" ]]; then
    tar -xzf "${sdl_archive}" -C "${source_dir}"
fi
cmake -S "${sdl_source}" -B "${work_dir}/sdl" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build "${work_dir}/sdl" --parallel "${jobs}"
cmake --install "${work_dir}/sdl"

spirv_source="${source_dir}/SPIRV-Cross"
checkout_exact "https://github.com/KhronosGroup/SPIRV-Cross.git" \
    "${spirv_source}" "${spirv_cross_commit}"
cmake -S "${spirv_source}" -B "${work_dir}/spirv-cross" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_SHARED=OFF \
    -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF
cmake --build "${work_dir}/spirv-cross" --parallel "${jobs}"
cmake --install "${work_dir}/spirv-cross"

dxc_archive="${download_dir}/${dxc_asset}"
download_verified "${dxc_url}" "${dxc_archive}" "${dxc_sha256}"
dxc_root="${prefix}/dxc-${dxc_version}"
if [[ ! -f "${dxc_root}/lib/libdxcompiler.so" ]]; then
    mkdir -p "${dxc_root}"
    tar -xzf "${dxc_archive}" -C "${dxc_root}"
fi

shadercross_source="${source_dir}/SDL_shadercross"
checkout_exact "https://github.com/libsdl-org/SDL_shadercross.git" \
    "${shadercross_source}" "${shadercross_commit}"
mkdir -p "${shadercross_source}/external"
if [[ ! -e "${shadercross_source}/external/DirectXShaderCompiler-binaries" ]]; then
    ln -s "${dxc_root}" \
        "${shadercross_source}/external/DirectXShaderCompiler-binaries"
fi
cmake -S "${shadercross_source}" -B "${work_dir}/shadercross" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DCMAKE_PREFIX_PATH="${prefix}" -DSDLSHADERCROSS_DXC=ON \
    -DSDLSHADERCROSS_VENDORED=OFF -DSDLSHADERCROSS_SHARED=OFF \
    -DSDLSHADERCROSS_STATIC=ON -DSDLSHADERCROSS_SPIRVCROSS_SHARED=OFF \
    -DSDLSHADERCROSS_CLI=OFF -DSDLSHADERCROSS_TESTS=OFF
cmake --build "${work_dir}/shadercross" --parallel "${jobs}"
cmake --install "${work_dir}/shadercross"

{
    printf 'SDL=%s\n' "${sdl_version}"
    printf 'SDL_SHA256=%s\n' "${sdl_sha256}"
    printf 'SDL_SHADERCROSS=%s\n' "${shadercross_commit}"
    printf 'SPIRV_CROSS=%s\n' "${spirv_cross_commit}"
    printf 'SPIRV_HEADERS=%s\n' "${spirv_headers_commit}"
    printf 'SPIRV_TOOLS=%s\n' "${spirv_tools_commit}"
    printf 'DXC_SOURCE=%s\n' "${dxc_source_commit}"
    printf 'DXC_RELEASE=%s\n' "${dxc_version}"
    printf 'DXC_SHA256=%s\n' "${dxc_sha256}"
} >"${prefix}/versions.txt"

printf 'SDL_GPU developer dependencies installed in %s\n' "${prefix}"
printf 'Use LD_LIBRARY_PATH=%s/lib for libdxcompiler.so at runtime.\n' "${dxc_root}"
