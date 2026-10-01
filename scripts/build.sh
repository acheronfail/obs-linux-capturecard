#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
command -v flatpak >/dev/null || { echo 'Flatpak is required.' >&2; exit 1; }
flatpak info com.obsproject.Studio >/dev/null
sdk_ref=$(flatpak info --show-sdk com.obsproject.Studio)
flatpak info "$sdk_ref" >/dev/null || {
    echo "Install the matching SDK first: flatpak install flathub $sdk_ref" >&2
    exit 1
}
flatpak run --devel --filesystem="$repo_dir" --command=sh com.obsproject.Studio -eu -c '
    export PKG_CONFIG_PATH=/app/lib/pkgconfig
    export LD_LIBRARY_PATH=/app/lib
    cmake -S "$1" -B "$1/build/flatpak" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=/app
    cmake --build "$1/build/flatpak"
    ctest --test-dir "$1/build/flatpak" --output-on-failure
' sh "$repo_dir"
