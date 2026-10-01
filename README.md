# OBS Linux Capture Card

An OBS source for Linux combining V4L2 video and USB audio, with Activate/Deactivate controls in the source toolbar and properties.

## Install

Requires `just`, Flatpak OBS (`com.obsproject.Studio`), and its matching Freedesktop SDK. The build script reports how to install a missing SDK.

```sh
just install    # Build, test, and install for the current user
```

Restart OBS, then add **Linux Capture Card (Audio + Video)**. Select your video device; audio is matched automatically by USB identity or can be selected manually. Remove any previous sources using the same card to avoid device conflicts and duplicate audio.

Use **Deactivate**, then **Activate** to reopen capture after HDMI signal changes. **Restart capture** is also available in Properties. Hiding the source does not release the device.

## Development

```sh
just build     # Build and run automated tests
just uninstall # Uninstall; restart OBS afterwards
```

For an optional hardware test, close other sources using the card, then run `just smoke /dev/v4l/by-id/<device>-video-index0`.

License: [AGPL-3.0-only](LICENSE).
