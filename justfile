default:
    @just --list

# Build against the installed Flatpak OBS and run lifecycle tests.
build:
    ./scripts/build.sh

# Install the plugin for the current user's Flatpak OBS. Restart OBS afterwards.
install: build
    ./scripts/install.sh

# Remove only this plugin from the current user's Flatpak OBS.
uninstall:
    ./scripts/uninstall.sh

test: build

# Opt-in hardware test; close other sources using this video device first.
smoke device:
    ./scripts/smoke.sh {{quote(device)}}
