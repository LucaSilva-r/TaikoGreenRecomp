#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

export PS3_VFS_ROOT="${PS3_VFS_ROOT:-${repo_dir}/game/vfs}"
export PS3_TOC_SET="${PS3_TOC_SET:-0x1027c58,0x1037a88,0x1047a38}"
export FLOW_NOSPILL="${FLOW_NOSPILL:-1}"
export TAIKO_DNS_LOOPBACK="${TAIKO_DNS_LOOPBACK:-1}"
export TAIKO_OFFLINE_COMPLETE="${TAIKO_OFFLINE_COMPLETE:-1}"
export TAIKO_FS_YIELD="${TAIKO_FS_YIELD:-0}"
export PS3RECOMP_NULL_RSX="${PS3RECOMP_NULL_RSX:-1}"
export PS3RECOMP_NULL_AUDIO="${PS3RECOMP_NULL_AUDIO:-1}"
export TAIKO_AUDIO_DECODE="${TAIKO_AUDIO_DECODE:-0}"
export TAIKO_AUDIO_SPU="${TAIKO_AUDIO_SPU:-0}"

exec "${repo_dir}/build-linux/taiko_boot" \
    "${repo_dir}/game/EBOOT.recomp.elf" "$@"
