#!/usr/bin/env bash
set -euo pipefail
# Usage: scripts/build_osu_lazer_reader.sh [output-directory] [runtime-id]
# A runtime-id (linux-x64, linux-arm64, win-x64) produces a self-contained
# executable; omit it for a development build requiring .NET 8.
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
output=${1:-"$project_root/build-linux/tools/osu_lazer_reader"}
args=()
if [[ -n ${2:-} ]]; then
    args+=(--runtime "$2" --self-contained true)
fi
dotnet publish "$project_root/tools/osu_lazer_reader/OsuLazerReader.csproj" \
    --configuration Release --output "$output" "${args[@]}"
