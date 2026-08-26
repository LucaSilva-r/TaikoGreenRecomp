#!/usr/bin/env bash
# Build the ARM64 Android APK with the pinned repo-local SDK and graphics stack.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sdk_root="${TAIKO_ANDROID_SDK:-${repo_dir}/third_party/android-sdk}"
ndk_root="${TAIKO_ANDROID_NDK:-${sdk_root}/ndk/28.2.13676358}"
sdl_root="${repo_dir}/third_party/sdl-gpu-rpi-arm64/.src/SDL3-3.4.10"
sdl_gpu_root="${TAIKO_SDL_GPU_ROOT:-${repo_dir}/third_party/sdl-gpu-android-arm64}"
gradle_version=9.1.0
gradle_sha256=a17ddd85a26b6a7f5ddb71ff8b05fc5104c0202c6e64782429790c933686c806
gradle_root="${repo_dir}/third_party/gradle-${gradle_version}"

[[ -f "${ndk_root}/build/cmake/android.toolchain.cmake" ]] || {
    echo "Android NDK 28.2.13676358 is missing; run scripts/setup_android_sdk.sh" >&2
    exit 2
}
[[ -d "${sdl_root}/android-project/app/src/main/java/org/libsdl/app" ]] || {
    echo "Pinned SDL Android Java sources are missing" >&2
    exit 2
}

if [[ ! -x "${gradle_root}/bin/gradle" ]]; then
    command -v curl >/dev/null || { echo "curl is required" >&2; exit 2; }
    command -v unzip >/dev/null || { echo "unzip is required" >&2; exit 2; }
    download_dir="${repo_dir}/third_party/downloads"
    gradle_zip="${download_dir}/gradle-${gradle_version}-bin.zip"
    mkdir -p "${download_dir}"
    if [[ ! -f "${gradle_zip}" ]]; then
        curl -fL --retry 3 -o "${gradle_zip}" \
            "https://services.gradle.org/distributions/gradle-${gradle_version}-bin.zip"
    fi
    printf '%s  %s\n' "${gradle_sha256}" "${gradle_zip}" | sha256sum -c -
    unzip -q -o "${gradle_zip}" -d "${repo_dir}/third_party"
fi

export ANDROID_HOME="${sdk_root}"
export ANDROID_SDK_ROOT="${sdk_root}"
export TAIKO_ANDROID_NDK="${ndk_root}"
export GRADLE_USER_HOME="${TAIKO_GRADLE_HOME:-${repo_dir}/third_party/gradle-home}"
export TAIKO_MBEDTLS_JOBS="${TAIKO_DEP_JOBS:-4}"
export TAIKO_FREETYPE_JOBS="${TAIKO_DEP_JOBS:-4}"

if [[ -z "${JAVA_HOME:-}" ]] &&
   [[ -x /usr/lib/jvm/java-21-openjdk/bin/java ]]; then
    export JAVA_HOME=/usr/lib/jvm/java-21-openjdk
    export PATH="${JAVA_HOME}/bin:${PATH}"
fi
java_major="$(java -version 2>&1 | sed -n '1s/.*version "\([0-9]*\).*/\1/p')"
if [[ -z "${java_major}" ]] || (( java_major < 17 || java_major > 25 )); then
    echo "Android Gradle requires JDK 17-25" >&2
    exit 2
fi

if [[ ! -f "${repo_dir}/third_party/mbedtls-android-arm64/lib/libmbedtls.a" ]]; then
    "${repo_dir}/scripts/build_mbedtls.sh" android-arm64
fi
if [[ ! -f "${repo_dir}/third_party/freetype-android-arm64/lib/libfreetype.a" ]]; then
    "${repo_dir}/scripts/build_freetype.sh" android-arm64
fi

headless="${TAIKO_ANDROID_HEADLESS:-0}"
if [[ "${headless}" == 1 ]]; then
    native_build="${repo_dir}/build-android-smoke"
    backend_args=(
        -DTAIKO_HEADLESS=ON
    )
else
    [[ -f "${sdl_gpu_root}/lib/libSDL3.so" &&
       -f "${sdl_gpu_root}/lib/libSDL3_shadercross.a" &&
       -f "${sdl_gpu_root}/dxc-v1.8.2502/lib/libdxcompiler.so" ]] || {
        echo "Android SDL_GPU dependencies are missing; run scripts/setup_sdl_gpu_android_arm64.sh" >&2
        exit 2
    }
    native_build="${repo_dir}/build-android"
    backend_args=(
        -DTAIKO_HEADLESS=OFF
        -DTAIKO_RSX_BACKEND=sdl_gpu
        -DTAIKO_INPUT_BACKEND=sdl3
        -DTAIKO_AUDIO_BACKEND=null
        -DTAIKO_SDL_GPU_ROOT="${sdl_gpu_root}"
    )
fi
cmake -S "${repo_dir}" -B "${native_build}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${ndk_root}/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-28 \
    -DCMAKE_BUILD_TYPE=Release \
    "${backend_args[@]}" \
    -DTAIKO_INPROCESS_ATRAC=OFF \
    -DTAIKO_EMBED_PPU_IMAGE=ON \
    -DTAIKO_MBEDTLS_ROOT="${repo_dir}/third_party/mbedtls-android-arm64" \
    -DTAIKO_FREETYPE_ROOT="${repo_dir}/third_party/freetype-android-arm64" \
    -DTAIKO_COMPILE_JOBS="${TAIKO_COMPILE_JOBS:-2}" \
    -DBUILD_TESTING=OFF
cmake --build "${native_build}" --target taiko_boot \
    --parallel "${TAIKO_DEP_JOBS:-4}"

jni_dir="${repo_dir}/android/app/src/main/jniLibs/arm64-v8a"
cmake -E make_directory "${jni_dir}"
cmake -E copy_if_different "${native_build}/libmain.so" "${jni_dir}/libmain.so"
if [[ "${headless}" == 1 && -f "${native_build}/android-sdl/libSDL3.so" ]]; then
    sdl_library="${native_build}/android-sdl/libSDL3.so"
else
    sdl_library="${sdl_gpu_root}/lib/libSDL3.so"
fi
cmake -E copy_if_different "${sdl_library}" "${jni_dir}/libSDL3.so"

runtime_libraries=("${jni_dir}/libmain.so" "${jni_dir}/libSDL3.so")
if [[ "${headless}" != 1 ]]; then
    for library in libdxcompiler.so libdxil.so; do
        cmake -E copy_if_different \
            "${sdl_gpu_root}/dxc-v1.8.2502/lib/${library}" \
            "${jni_dir}/${library}"
        runtime_libraries+=("${jni_dir}/${library}")
    done
fi
"${ndk_root}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" \
    --strip-unneeded "${runtime_libraries[@]}"

printf 'sdk.dir=%s\ncmake.path=%s\n' "${sdk_root}" "$(command -v cmake)" \
    > "${repo_dir}/android/local.properties"

"${gradle_root}/bin/gradle" \
    --project-dir "${repo_dir}/android" \
    --no-daemon \
    :app:assembleDebug

apk="${repo_dir}/android/app/build/outputs/apk/debug/app-debug.apk"
[[ -f "${apk}" ]] || { echo "APK was not produced: ${apk}" >&2; exit 1; }
printf 'Android APK: %s\n' "${apk}"
