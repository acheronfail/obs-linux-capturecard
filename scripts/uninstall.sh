#!/usr/bin/env bash
set -euo pipefail
plugin_dir="$HOME/.var/app/com.obsproject.Studio/config/obs-studio/plugins/capture-card"
rm -f -- "$plugin_dir/bin/64bit/capture-card.so" "$plugin_dir/bin/64bit/capture-card.so.new"
for dir in "$plugin_dir/bin/64bit" "$plugin_dir/bin" "$plugin_dir/data" "$plugin_dir"; do
    if [ -d "$dir" ]; then rmdir -- "$dir" 2>/dev/null || true; fi
done
echo 'Uninstalled capture-card. Restart OBS. Scene collections have not been edited.'
