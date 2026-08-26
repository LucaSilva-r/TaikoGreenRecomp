#!/usr/bin/env bash
# Install and launch the ARM64 Android proof of concept on an attached device.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sdk_root="${TAIKO_ANDROID_SDK:-${repo_dir}/third_party/android-sdk}"
adb="${TAIKO_ADB:-${sdk_root}/platform-tools/adb}"
apk="${TAIKO_ANDROID_APK:-${repo_dir}/android/app/build/outputs/apk/debug/app-debug.apk}"
package=org.taikorecomp.app
activity="${package}/.TaikoActivity"
device_vfs="/sdcard/Android/data/${package}/files/vfs"
push_data=0

usage() {
    echo "usage: $0 [--push-data]" >&2
    echo "  --push-data  copy the local game USRDIR contents (about 16 GiB)" >&2
}

case "${1:-}" in
    "") ;;
    --push-data) push_data=1 ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 2 ;;
esac

[[ -x "${adb}" ]] || { echo "adb is missing: ${adb}" >&2; exit 2; }
[[ -f "${apk}" ]] || {
    echo "APK is missing; run scripts/build_android_arm64.sh first" >&2
    exit 2
}

serial="$(${adb} get-serialno)"
[[ -n "${serial}" && "${serial}" != unknown ]] || {
    echo "No authorized Android device found" >&2
    exit 2
}

abi="$(${adb} shell getprop ro.product.cpu.abi | tr -d '\r')"
api="$(${adb} shell getprop ro.build.version.sdk | tr -d '\r')"
[[ "${abi}" == arm64-v8a ]] || {
    echo "Device ${serial} uses ${abi}; this APK contains arm64-v8a only" >&2
    exit 2
}
(( api >= 28 )) || {
    echo "Device API ${api} is below the APK minimum (28)" >&2
    exit 2
}

echo "Installing on ${serial} (${abi}, API ${api})"
${adb} install -r "${apk}"

# One launch lets Android create the app-specific external-files directory.
${adb} shell am start -W -n "${activity}" >/dev/null
${adb} shell am force-stop "${package}"

if (( push_data )); then
    source_usrdir="$(readlink -f "${repo_dir}/game/vfs/PS3_GAME/USRDIR")"
    [[ -d "${source_usrdir}" && -f "${source_usrdir}/DATA00000.BIN" ]] || {
        echo "Game USRDIR is missing or incomplete: ${source_usrdir}" >&2
        exit 2
    }
    ${adb} shell mkdir -p "${device_vfs}"
    echo "Copying USRDIR to ${device_vfs}; this is about 16 GiB"
    ${adb} push "${source_usrdir}/." "${device_vfs}/"
fi

${adb} logcat -c
${adb} shell am start -W -n "${activity}"
echo "Launched ${package}. Native output is available through adb logcat."
