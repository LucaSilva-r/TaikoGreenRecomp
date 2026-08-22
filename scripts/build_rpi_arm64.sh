#!/usr/bin/env bash
# Reproducible Raspberry Pi OS 64-bit cross-build entry point.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
engine="${CONTAINER_ENGINE:-}"
if [[ -z "${engine}" ]]; then
    if command -v docker >/dev/null 2>&1; then engine=docker
    elif command -v podman >/dev/null 2>&1; then engine=podman
    else
        echo "Podman or Docker is required." >&2
        exit 2
    fi
fi

image="taikorecomp-rpi-arm64-builder:bookworm"
"${engine}" build -t "${image}" \
    -f "${repo_dir}/containers/raspberry-pi-arm64.Dockerfile" \
    "${repo_dir}/containers"
"${engine}" run --rm --security-opt label=disable \
    --user "$(id -u):$(id -g)" \
    -e TAIKO_COMPILE_JOBS="${TAIKO_COMPILE_JOBS:-4}" \
    -e TAIKO_DEP_JOBS="${TAIKO_DEP_JOBS:-4}" \
    -e TAIKO_RPI_HEADLESS="${TAIKO_RPI_HEADLESS:-0}" \
    -e TAIKO_RPI_BUILD_REPLAY="${TAIKO_RPI_BUILD_REPLAY:-0}" \
    -v "${repo_dir}:/src" -w /src "${image}" \
    ./scripts/build_rpi_arm64_inner.sh
