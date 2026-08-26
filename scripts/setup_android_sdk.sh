#!/usr/bin/env bash
# Install the pinned Android command-line SDK and ARM64 NDK under third_party/.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sdk_root="${TAIKO_ANDROID_SDK:-${repo_dir}/third_party/android-sdk}"
download_dir="${repo_dir}/third_party/downloads"
tools_revision=15859902
tools_zip="${download_dir}/commandlinetools-linux-${tools_revision}_latest.zip"
tools_url="https://dl.google.com/android/repository/commandlinetools-linux-${tools_revision}_latest.zip"
tools_sha256=4e4c464f145a7512b57d088ac6c278c03c9eea610886b35a5e0804e74eedf583

for tool in curl sha256sum unzip; do
    command -v "${tool}" >/dev/null || {
        echo "Missing required tool: ${tool}" >&2
        exit 2
    }
done

mkdir -p "${sdk_root}/cmdline-tools" "${download_dir}"
if [[ ! -x "${sdk_root}/cmdline-tools/latest/bin/sdkmanager" ]]; then
    if [[ ! -f "${tools_zip}" ]]; then
        curl -fL --retry 3 -o "${tools_zip}" "${tools_url}"
    fi
    printf '%s  %s\n' "${tools_sha256}" "${tools_zip}" | sha256sum -c -

    extract_dir="${sdk_root}/cmdline-tools/.install-${tools_revision}"
    mkdir -p "${extract_dir}"
    unzip -q -o "${tools_zip}" -d "${extract_dir}"
    mv "${extract_dir}/cmdline-tools" "${sdk_root}/cmdline-tools/latest"
    rmdir "${extract_dir}"
fi

sdkmanager="${sdk_root}/cmdline-tools/latest/bin/sdkmanager"
echo "Review and accept the Android SDK licenses when prompted."
"${sdkmanager}" --sdk_root="${sdk_root}" --licenses
"${sdkmanager}" --sdk_root="${sdk_root}" \
    "platforms;android-35" \
    "build-tools;36.0.0" \
    "platform-tools" \
    "ndk;28.2.13676358"

echo "Android SDK installed in ${sdk_root}"
