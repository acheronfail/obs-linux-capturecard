#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
plugin_dir="$HOME/.var/app/com.obsproject.Studio/config/obs-studio/plugins/capture-card"
test -f "$repo_dir/build/flatpak/capture-card.so" || { echo 'Run just build first.' >&2; exit 1; }
mkdir -p "$plugin_dir/bin/64bit" "$plugin_dir/data"
# Atomic replacement also permits installing while OBS still has the old file mapped.
install -m 755 "$repo_dir/build/flatpak/capture-card.so" "$plugin_dir/bin/64bit/capture-card.so.new"
mv -f "$plugin_dir/bin/64bit/capture-card.so.new" "$plugin_dir/bin/64bit/capture-card.so"
echo "Installed: $plugin_dir"
echo 'Restart OBS, then add a Linux Capture Card (Audio + Video) source.'
