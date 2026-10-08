# OBS Linux Capture Card

An OBS source for Linux combining V4L2 video and USB audio, with Activate/Deactivate controls in the source toolbar and properties.

## Install

Requires `just`, Flatpak OBS (`com.obsproject.Studio`), and its matching Freedesktop SDK. The build script reports how to install a missing SDK.

```sh
just install    # Build, test, and install for the current user
```

Restart OBS, then add **Linux Capture Card (Audio + Video)**. Select your video device; audio is matched automatically by USB identity or can be selected manually. Remove any previous sources using the same card to avoid device conflicts and duplicate audio.

Capture automatically reopens the configured stable device after USB disconnects or device-node replacement. If video timestamps stop advancing for five seconds, it retries at most once every ten seconds. Manually deactivated sources stay off; hiding a source does not release the device.

The toolbar shows **Stopping…** or **Starting…** until backend cleanup or startup finishes. Properties report **receiving frames** only when a new frame timestamp was observed within the last two seconds. **Restart capture** is available for manual recovery after HDMI signal changes.

OBS logs include recovery reasons and timings for lifecycle queue waits, audio callback removal, backend reference release, the OBS destruction barrier, and backend creation.

## Development

```sh
just build     # Build and run frame-health, USB lifecycle, and toolbar tests
just uninstall # Uninstall; restart OBS afterwards
```

For an optional hardware test, close other sources using the card, then run `just smoke /dev/v4l/by-id/<device>-video-index0`.

## Investigating signal-loss failures

Before reconnecting a failed card, run
`just diagnose /dev/v4l/by-id/<device>-video-index0 > capture-diagnostics.txt`.
This reads USB topology, power-management counters, device ownership, recent
kernel events, and capture-related OBS logs without opening or resetting capture.
Run it on the host; a sandbox may hide device nodes or kernel events.
Reports contain local paths and device identifiers; review before sharing.

Recovery logs include the monotonic timestamp, frame age (`-1` means no frame
observed), device-node identity, and lifecycle request reason. These help establish
whether a USB failure preceded a plugin restart. Creating a backend object does
not prove that its capture thread successfully started.

For HDMI-transition failures, compare the same console power/input change with
this plugin and OBS's standard **Video Capture Device (V4L2)** source, using the
same format, size, and frame rate. Deactivate this plugin before opening the
standard source, and release the standard source before reactivating the plugin.
Keep USB connected during the comparison. Record the trigger and time, and collect
diagnostics immediately after a failure. If both fail, investigate the card,
USB connection, and kernel driver; if only this plugin fails repeatedly, compare
its lifecycle logs. A successful single trial does not establish reliability.

An `ENODEV` ("No such device") failure while queuing buffers can occur even when
the device node still exists. A USB reconnect can recover a state that reopening
the V4L2 source cannot. Automatic USB resets and persistent power-management
changes are not performed by this plugin.

License: [AGPL-3.0-only](LICENSE).
