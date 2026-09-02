#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="${TAIKO_SDL_GPU_ROOT:-${repo_dir}/third_party/sdl-gpu-mingw}"
source_dir="${prefix}/.src"
work_dir="${prefix}/.build"
download_dir="${prefix}/.downloads"
toolchain="${repo_dir}/cmake/mingw-w64-dependencies.cmake"
jobs="${TAIKO_DEP_JOBS:-4}"

sdl_version="3.4.10"
sdl_url="https://github.com/libsdl-org/SDL/releases/download/release-${sdl_version}/SDL3-${sdl_version}.tar.gz"
sdl_sha256="12b34280415ec8418c864408b93d008a20a6530687ee613d60bfbd20411f2785"
shadercross_commit="e55cf5e31ced6f3d1be5cc6d0c50e99384f9f4ba"
spirv_cross_commit="1a6169566c73d3da552748fc372fe2bbb856e46e"
dxc_version="v1.8.2502"
dxc_asset="dxc_2025_02_20.zip"
dxc_url="https://github.com/microsoft/DirectXShaderCompiler/releases/download/${dxc_version}/${dxc_asset}"
dxc_sha256="70b1913a1bfce4a3e1a5311d16246f4ecdf3a3e613abec8aa529e57668426f85"
agility_version="1.619.5"
agility_asset="Microsoft.Direct3D.D3D12.${agility_version}.nupkg"
agility_url="https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/${agility_version}"
agility_sha256="0e9bcf32aac9a79343ede9b21e4864950ee54577e3d8e19bfcdf002bb4e9bfd6"

for tool in cmake git ninja curl sha256sum tar unzip \
            x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++; do
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
    -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build "${work_dir}/sdl" --parallel "${jobs}"
cmake --install "${work_dir}/sdl"

spirv_source="${source_dir}/SPIRV-Cross"
checkout_exact "https://github.com/KhronosGroup/SPIRV-Cross.git" \
    "${spirv_source}" "${spirv_cross_commit}"
cmake -S "${spirv_source}" -B "${work_dir}/spirv-cross" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_SHARED=OFF \
    -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF
cmake --build "${work_dir}/spirv-cross" --parallel "${jobs}"
cmake --install "${work_dir}/spirv-cross"

dxc_archive="${download_dir}/${dxc_asset}"
download_verified "${dxc_url}" "${dxc_archive}" "${dxc_sha256}"
dxc_root="${prefix}/dxc-${dxc_version}"
if [[ ! -f "${dxc_root}/bin/x64/dxcompiler.dll" ]]; then
    mkdir -p "${dxc_root}"
    unzip -q "${dxc_archive}" -d "${dxc_root}"
fi

# SDL's D3D12 backend needs newer runtime feature queries than older Windows
# installations expose. Vendor Microsoft's official Agility SDK under the
# required subdirectory instead of placing D3D12Core.dll beside the executable.
agility_archive="${download_dir}/${agility_asset}"
download_verified "${agility_url}" "${agility_archive}" "${agility_sha256}"
agility_root="${prefix}/d3d12-agility-${agility_version}"
if [[ ! -f "${agility_root}/bin/x64/D3D12Core.dll" ||
      ! -f "${agility_root}/bin/x64/d3d12SDKLayers.dll" ]]; then
    mkdir -p "${agility_root}/bin/x64" "${agility_root}/licenses"
    unzip -j -o "${agility_archive}" \
        build/native/bin/x64/D3D12Core.dll \
        build/native/bin/x64/d3d12SDKLayers.dll \
        -d "${agility_root}/bin/x64"
    unzip -j -o "${agility_archive}" LICENSE.txt LICENSE-CODE.txt \
        -d "${agility_root}/licenses"
fi

shadercross_source="${source_dir}/SDL_shadercross"
checkout_exact "https://github.com/libsdl-org/SDL_shadercross.git" \
    "${shadercross_source}" "${shadercross_commit}"
mkdir -p "${shadercross_source}/external"
if [[ ! -e "${shadercross_source}/external/DirectXShaderCompiler-binaries" ]]; then
    ln -s "${dxc_root}" \
        "${shadercross_source}/external/DirectXShaderCompiler-binaries"
fi
cmake -S "${shadercross_source}" -B "${work_dir}/shadercross-mingw" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DCMAKE_PREFIX_PATH="${prefix}" \
    -DDirectXShaderCompiler_INCLUDE_PATH="${dxc_root}/inc" \
    -DDirectXShaderCompiler_dxcompiler_BINARY="${dxc_root}/bin/x64/dxcompiler.dll" \
    -DDirectXShaderCompiler_dxcompiler_LIBRARY="${dxc_root}/lib/x64/dxcompiler.lib" \
    -DDirectXShaderCompiler_dxil_BINARY="${dxc_root}/bin/x64/dxil.dll" \
    -DSDLSHADERCROSS_DXC=ON -DSDLSHADERCROSS_VENDORED=OFF \
    -DSDLSHADERCROSS_SHARED=OFF -DSDLSHADERCROSS_STATIC=ON \
    -DSDLSHADERCROSS_SPIRVCROSS_SHARED=OFF \
    -DSDLSHADERCROSS_CLI=OFF -DSDLSHADERCROSS_TESTS=OFF
cmake --build "${work_dir}/shadercross-mingw" --parallel "${jobs}"
cmake --install "${work_dir}/shadercross-mingw"

{
    printf 'TARGET=windows-x86_64-mingw\n'
    printf 'SDL=%s\n' "${sdl_version}"
    printf 'SDL_SHA256=%s\n' "${sdl_sha256}"
    printf 'SDL_SHADERCROSS=%s\n' "${shadercross_commit}"
    printf 'SPIRV_CROSS=%s\n' "${spirv_cross_commit}"
    printf 'DXC_RELEASE=%s\n' "${dxc_version}"
    printf 'DXC_SHA256=%s\n' "${dxc_sha256}"
    printf 'D3D12_AGILITY=%s\n' "${agility_version}"
    printf 'D3D12_AGILITY_SDK_VERSION=619\n'
    printf 'D3D12_AGILITY_SHA256=%s\n' "${agility_sha256}"
} >"${prefix}/versions.txt"

printf 'MinGW SDL_GPU dependencies installed in %s\n' "${prefix}"
printf 'The game build copies DXC beside the executable and Agility SDK into D3D12/.\n'
