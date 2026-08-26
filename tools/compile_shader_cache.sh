#!/usr/bin/env bash
# Compile HLSL cache misses emitted by the SDL_GPU backend into portable SPIR-V.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cache_dir="${1:-}"
dxc="${TAIKO_DXC:-${repo_dir}/third_party/sdl-gpu-linux/dxc-v1.8.2502/bin/dxc}"

if [[ -z "${cache_dir}" || ! -d "${cache_dir}" ]]; then
    echo "usage: $0 CACHE_DIRECTORY" >&2
    exit 2
fi
if [[ ! -x "${dxc}" ]]; then
    echo "DXC executable not found: ${dxc}" >&2
    exit 2
fi

shopt -s nullglob
compiled=0
for source in "${cache_dir}"/*.hlsl; do
    output="${source%.hlsl}.spv"
    [[ -f "${output}" ]] && continue
    base="$(basename -- "${source}")"
    case "${base}" in
        v*-v-*.hlsl) profile=vs_6_0 ;;
        v*-f-*.hlsl) profile=ps_6_0 ;;
        *)
            echo "cannot infer shader stage from ${base}" >&2
            exit 2
            ;;
    esac
    "${dxc}" -E main -T "${profile}" \
        -spirv -fspv-flatten-resource-arrays \
        -fspv-preserve-bindings -fspv-preserve-interface \
        -Fo "${output}" "${source}"
    echo "compiled ${base} -> $(basename -- "${output}")"
    ((compiled += 1))
done
echo "compiled ${compiled} shader(s)"
