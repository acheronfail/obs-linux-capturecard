#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test "$#" -eq 1 || { echo 'Usage: just smoke /dev/v4l/by-id/...-video-index0' >&2; exit 2; }
test -x "$repo_dir/build/flatpak/capture-card-smoke" || { echo 'Run just build first.' >&2; exit 1; }
if command -v fuser >/dev/null && fuser "$1" >/dev/null 2>&1; then
    echo 'The video device is in use. Deactivate its sources or close OBS before running this test.' >&2
    exit 1
fi
flatpak run --devel --filesystem="$repo_dir" --command=sh com.obsproject.Studio -eu -c '
    export LD_LIBRARY_PATH=/app/lib
    timeout 60 "$1/build/flatpak/capture-card-smoke" "$1/build/flatpak/capture-card.so" "$2"
' sh "$repo_dir" "$1"
