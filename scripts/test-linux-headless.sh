#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${TAIKO_LINUX_BINARY:-${repo_dir}/build-linux/taiko_boot}"
marker="/data/lumendata/packed/attract/title/packeddata.ddp"
bad='TOCBAD|unresolved indirect|assertion failed|xml-fatal|ignored impossible free|invalid-free|\[CRASH\]|\[ABORT\]|Abort|abort'
timeout_seconds="${TAIKO_HEADLESS_TIMEOUT:-240}"
soak_seconds="${TAIKO_HEADLESS_SOAK:-15}"
log_dir="${TAIKO_HEADLESS_LOG_DIR:-${repo_dir}/build-linux/headless-test}"

mkdir -p "${log_dir}"
if [[ ! -x "${binary}" ]]; then
    printf 'Missing native executable: %s\n' "${binary}" >&2
    exit 2
fi

terminate_pid() {
    local pid="$1"
    kill -TERM "${pid}" 2>/dev/null || true
    for _ in {1..20}; do
        kill -0 "${pid}" 2>/dev/null || return 0
        sleep 0.1
    done
    kill -KILL "${pid}" 2>/dev/null || true
}

for boot in 1 2 3; do
    log="${log_dir}/boot-${boot}.log"
    : >"${log}"
    PS3_VFS_ROOT="${PS3_VFS_ROOT:-${repo_dir}/game/vfs}" \
    PS3_TOC_SET="${PS3_TOC_SET:-0x1027c58,0x1037a88,0x1047a38}" \
    FLOW_NOSPILL=1 TAIKO_DNS_LOOPBACK=1 TAIKO_OFFLINE_COMPLETE=1 \
    TAIKO_FS_YIELD=0 PS3RECOMP_NULL_RSX=1 PS3RECOMP_NULL_AUDIO=1 \
    TAIKO_AUDIO_DECODE=0 TAIKO_AUDIO_SPU=0 \
        "${binary}" "${repo_dir}/game/EBOOT.elf" >"${log}" 2>&1 &
    pid=$!
    trap 'terminate_pid "${pid:-0}"' EXIT INT TERM

    deadline=$((SECONDS + timeout_seconds))
    while ! grep -Fq "${marker}" "${log}"; do
        if ! kill -0 "${pid}" 2>/dev/null; then
            wait "${pid}" || true
            printf 'Boot %d exited before attract; see %s\n' "${boot}" "${log}" >&2
            exit 1
        fi
        if (( SECONDS >= deadline )); then
            terminate_pid "${pid}"
            printf 'Boot %d timed out; see %s\n' "${boot}" "${log}" >&2
            exit 1
        fi
        sleep 1
    done

    progress_before=$(grep -c '\[HEADLESS-PROGRESS\]' "${log}" || true)
    sleep "${soak_seconds}"
    progress_after=$(grep -c '\[HEADLESS-PROGRESS\]' "${log}" || true)
    if ! kill -0 "${pid}" 2>/dev/null || (( progress_after <= progress_before )); then
        terminate_pid "${pid}"
        printf 'Boot %d stopped progressing during soak; see %s\n' "${boot}" "${log}" >&2
        exit 1
    fi
    if grep -Eiq "${bad}" "${log}"; then
        terminate_pid "${pid}"
        printf 'Boot %d contains a fatal marker; see %s\n' "${boot}" "${log}" >&2
        exit 1
    fi

    terminate_pid "${pid}"
    wait "${pid}" 2>/dev/null || true
    if kill -0 "${pid}" 2>/dev/null; then
        printf 'Boot %d left PID %d alive\n' "${boot}" "${pid}" >&2
        exit 1
    fi
    trap - EXIT INT TERM
    printf 'Boot %d passed (%s)\n' "${boot}" "${log}"
done
