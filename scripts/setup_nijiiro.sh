#!/usr/bin/env bash
set -euo pipefail
# Source-only dependencies, built for each target by CMake. No runtime helpers.
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
fetch_source() {
    local name=$1 url=$2 commit=$3
    local dest="$project_root/third_party/$name"
    if [[ ! -d "$dest/.git" ]]; then
        git init "$dest"
        git -C "$dest" remote add origin "$url"
        git -C "$dest" fetch --depth 1 origin "$commit"
        git -C "$dest" checkout --detach FETCH_HEAD
    fi
    if [[ $(git -C "$dest" rev-parse HEAD) != "$commit" ]]; then
        echo "$name does not match pinned revision $commit" >&2
        exit 1
    fi
}
fetch_source vgmstream-source https://github.com/vgmstream/vgmstream.git 09c9f40caae4747e44b6a993b3d5b654cef4d1f7
fetch_source libg719-source https://github.com/kode54/libg719_decode.git da90ad8a676876c6c47889bcea6a753f9bbf7a73
