#!/usr/bin/env bash
set -euo pipefail
# Source-only dependency: CMake builds the native storage engine for its target.
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
realm_source="$project_root/third_party/realm-core-source"
realm_commit=cccb3ca9e26ec452a29f2f0d4050d1e38b8a3d43
if [[ ! -d "$realm_source/.git" ]]; then
    git clone --depth 1 --branch v14.14.0 https://github.com/realm/realm-core.git "$realm_source"
fi
if [[ $(git -C "$realm_source" rev-parse HEAD) != "$realm_commit" ]]; then
    echo "Realm source does not match pinned v14.14.0 ($realm_commit)." >&2
    exit 1
fi
git -C "$realm_source" submodule update --init --depth 1 src/external/sha-1 src/external/sha-2
for realm_patch in "$project_root/scripts/patches/realm-no-restore-without-upgrade.patch" \
                   "$project_root/scripts/patches/realm-mingw.patch"; do
    if git -C "$realm_source" apply --check "$realm_patch" 2>/dev/null; then
        git -C "$realm_source" apply "$realm_patch"
    else
        git -C "$realm_source" apply --reverse --check "$realm_patch"
    fi
done
