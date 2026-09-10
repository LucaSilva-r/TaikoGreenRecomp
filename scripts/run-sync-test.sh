#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
test_dir="${repo_dir}/game/sync-test"
if [[ ! -f "${test_dir}/manifest.json" ]]; then
    echo 'Run tools/prepare_sync_test.py with the connector Python environment first.' >&2
    exit 1
fi
run_dir="${test_dir}/runs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "${run_dir}"
export PS3_VFS_ROOT="${test_dir}/vfs"
export PS3_VFS_LAYOUT=usrdir
export TAIKO_SONG_TITLES="${test_dir}/titles.tsv"
export TAIKO_LOG_FILE="${run_dir}/taiko.log"
export RSX_FPS_LOG=1
export RSX_FRAME_PACING_TRACE=1
export RSX_RESOURCE_TRACE=1
export TAIKO_AUDIO_SINK_TRACE=1
export TAIKO_AUDIO_LATENCY_TRACE=1
export TAIKO_AUDIO_GAMEPLAY_DUMP="${run_dir}/gameplay.wav"
export TAIKO_AUDIO_DUMP_SECONDS=600
if [[ "${TAIKO_SYNC_AUTO_HIT:-0}" == 1 ]]; then
    song_slot="$(python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); assert m.get("auto_tones"), "Regenerate with --auto-tones first"; print(m["song_slot"].upper())' "${test_dir}/manifest.json")"
    export TAIKO_SYNC_TEST_NUB_SOURCE="${PS3_VFS_ROOT}/data/sound/bgm/nub/SONG_${song_slot}.nub"
fi
cp "${test_dir}/manifest.json" "${run_dir}/manifest.json"
echo "Select SYNC TEST - 3 minutes (Kagerou Daze / mikukg), any difficulty."
echo "Test evidence: ${run_dir}"
exec "${repo_dir}/run-taiko-linux.sh" "$@"
