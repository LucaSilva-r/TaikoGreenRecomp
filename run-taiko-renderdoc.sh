#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
capture_dir="${RENDERDOC_CAPTURE_DIR:-${repo_dir}/build/renderdoc}"

mkdir -p "${capture_dir}"

# Fedora's RenderDoc package registers an implicit Vulkan layer which is
# opt-in.  Taiko uses D3D12 through vkd3d-proton, so the layer must be enabled
# in the Wine child that creates the translated Vulkan device.
export ENABLE_VULKAN_RENDERDOC_CAPTURE="${ENABLE_VULKAN_RENDERDOC_CAPTURE:-1}"
export TAIKO_LOG_FILE="${TAIKO_LOG_FILE:-${repo_dir}/build/taiko-renderdoc.log}"
export RENDERDOC_CAPFILE="${RENDERDOC_CAPFILE:-${capture_dir}/taiko}"

# Launch Wine directly. The Vulkan implicit layer is inherited by Wine's
# Vulkan process; renderdoccmd's Linux injector instead attaches to this shell
# and loses the Wine child during handoff.
exec "${repo_dir}/run-taiko.sh" "$@"
