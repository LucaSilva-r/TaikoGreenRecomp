#!/usr/bin/env bash
# Cross-build the complete SDL3/SDL_GPU shader stack for Raspberry Pi ARM64.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="${TAIKO_SDL_GPU_ROOT:-${repo_dir}/third_party/sdl-gpu-rpi-arm64}"
source_dir="${prefix}/.src"
work_dir="${prefix}/.build"
download_dir="${prefix}/.downloads"
toolchain="${repo_dir}/cmake/raspberry-pi-aarch64.cmake"
jobs="${TAIKO_DEP_JOBS:-4}"

sdl_version="3.4.10"
sdl_url="https://github.com/libsdl-org/SDL/releases/download/release-${sdl_version}/SDL3-${sdl_version}.tar.gz"
sdl_sha256="12b34280415ec8418c864408b93d008a20a6530687ee613d60bfbd20411f2785"
shadercross_commit="e55cf5e31ced6f3d1be5cc6d0c50e99384f9f4ba"
spirv_cross_commit="1a6169566c73d3da552748fc372fe2bbb856e46e"
dxc_commit="2c84a1c5ab7091608c97df6ba5ccf46e71c322eb"
dxc_root="${prefix}/dxc-v1.8.2502"

for tool in cmake git ninja curl sha256sum tar \
            aarch64-linux-gnu-gcc aarch64-linux-gnu-g++; do
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
sdl_dmabuf_patch="${repo_dir}/patches/SDL3-3.4.10-vulkan-dmabuf.patch"
if ! grep -q 'SDL_GPUVulkanExportTextureDMABUF' \
        "${sdl_source}/src/gpu/vulkan/SDL_gpu_vulkan.c"; then
    git -C "${sdl_source}" apply "${sdl_dmabuf_patch}"
fi
# KMSDRM stays off deliberately. SDL's KMSDRM video driver builds fine, but the
# SDL_GPU Vulkan backend then needs direct-to-display, and on the Pi the render
# device (v3d) is render-only while the display lives on a separate vc4 node --
# Vulkan enumerates no displays and initialization fails. A compositor is not
# optional here.
cmake -S "${sdl_source}" -B "${work_dir}/sdl" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DSDL_INSTALL=ON -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_X11=OFF -DSDL_WAYLAND=ON -DSDL_WAYLAND_LIBDECOR=OFF \
    -DSDL_WAYLAND_SHARED=ON -DSDL_VULKAN=ON -DSDL_KMSDRM=OFF \
    -DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON \
    -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF \
    -DSDL_SNDIO=OFF -DSDL_LIBUDEV=ON
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

# Linux DXC releases are x86-64-only. Build the exact source revision used by
# this ShaderCross checkout. DXC's CMake cross-build creates matching native
# TableGen helpers, then emits AArch64 libdxcompiler/libdxil shared libraries.
dxc_source="${source_dir}/DirectXShaderCompiler"
checkout_exact "https://github.com/libsdl-org/DirectXShaderCompiler.git" \
    "${dxc_source}" "${dxc_commit}"
git -C "${dxc_source}" submodule update --init --depth 1

# LLVM's generated headers need TableGen executables which run during the
# build. Build those two tools for the x86-64 container first, then point the
# AArch64 configuration at them. DXC's legacy automatic cross helper otherwise
# inherits CMAKE_TOOLCHAIN_FILE and mistakenly emits AArch64 host tools.
cmake -S "${dxc_source}" -B "${work_dir}/dxc-native" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DDXC_COVERAGE=OFF \
    -C "${dxc_source}/cmake/caches/PredefinedParams.cmake" \
    -DHLSL_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF \
    -DHLSL_DISABLE_SOURCE_GENERATION=TRUE -DSPIRV_BUILD_TESTS=OFF \
    -DLLVM_PARALLEL_COMPILE_JOBS="${jobs}" -DLLVM_PARALLEL_LINK_JOBS=1
cmake --build "${work_dir}/dxc-native" \
    --target llvm-tblgen clang-tblgen --parallel "${jobs}"

cmake -S "${dxc_source}" -B "${work_dir}/dxc-arm64" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${dxc_root}" \
    -DDXC_COVERAGE=OFF \
    -C "${dxc_source}/cmake/caches/PredefinedParams.cmake" \
    -DHLSL_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF \
    -DHLSL_DISABLE_SOURCE_GENERATION=TRUE -DSPIRV_BUILD_TESTS=OFF \
    -DLLVM_TABLEGEN="${work_dir}/dxc-native/bin/llvm-tblgen" \
    -DCLANG_TABLEGEN="${work_dir}/dxc-native/bin/clang-tblgen" \
    -DLLVM_PARALLEL_COMPILE_JOBS="${jobs}" -DLLVM_PARALLEL_LINK_JOBS=1
cmake --build "${work_dir}/dxc-arm64" --target dxcompiler dxildll \
    --parallel "${jobs}"
mkdir -p "${dxc_root}/include" "${dxc_root}/lib"
cmake -E copy_directory "${dxc_source}/include/dxc" "${dxc_root}/include/dxc"
cp -a "${work_dir}/dxc-arm64/lib/libdxcompiler.so"* "${dxc_root}/lib/"
cp -a "${work_dir}/dxc-arm64/lib/libdxil.so"* "${dxc_root}/lib/"

shadercross_source="${source_dir}/SDL_shadercross"
checkout_exact "https://github.com/libsdl-org/SDL_shadercross.git" \
    "${shadercross_source}" "${shadercross_commit}"
cmake -S "${shadercross_source}" -B "${work_dir}/shadercross" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DCMAKE_PREFIX_PATH="${prefix}" \
    -DDirectXShaderCompiler_ROOT="${dxc_root}" \
    -DDirectXShaderCompiler_INCLUDE_PATH="${dxc_root}/include/dxc" \
    -DDirectXShaderCompiler_dxcompiler_LIBRARY="${dxc_root}/lib/libdxcompiler.so" \
    -DDirectXShaderCompiler_dxil_LIBRARY="${dxc_root}/lib/libdxil.so" \
    -DSDLSHADERCROSS_DXC=ON -DSDLSHADERCROSS_VENDORED=OFF \
    -DSDLSHADERCROSS_SHARED=OFF -DSDLSHADERCROSS_STATIC=ON \
    -DSDLSHADERCROSS_SPIRVCROSS_SHARED=OFF \
    -DSDLSHADERCROSS_CLI=OFF -DSDLSHADERCROSS_TESTS=OFF
cmake --build "${work_dir}/shadercross" --parallel "${jobs}"
cmake --install "${work_dir}/shadercross"

{
    printf 'TARGET=linux-aarch64-rpi\n'
    printf 'SDL=%s\n' "${sdl_version}"
    printf 'SDL_SHA256=%s\n' "${sdl_sha256}"
    printf 'SDL_SHADERCROSS=%s\n' "${shadercross_commit}"
    printf 'SPIRV_CROSS=%s\n' "${spirv_cross_commit}"
    printf 'DXC_SOURCE=%s\n' "${dxc_commit}"
} >"${prefix}/versions.txt"

printf 'Raspberry Pi ARM64 SDL_GPU dependencies installed in %s\n' "${prefix}"
printf 'Deploy %s/lib/libdxcompiler.so* and libdxil.so* beside the game.\n' \
    "${dxc_root}"
