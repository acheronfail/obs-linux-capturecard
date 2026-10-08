#!/usr/bin/env python3
"""Read-only capture diagnostics; never opens, stops, or resets the device."""
import argparse
from datetime import datetime
from pathlib import Path
import os
import platform
import re
import subprocess


def command(args):
    print("\n$ " + " ".join(args), flush=True)
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=10)
        print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="")
        if result.returncode:
            print(f"(exit {result.returncode})")
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"Unavailable: {error}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", type=Path, help="Stable /dev/v4l/by-id/... path")
    args = parser.parse_args()
    print(f"Capture diagnostics: {datetime.now().astimezone().isoformat()}")
    print(f"Kernel: {platform.release()}")
    print(f"Requested device: {args.device}")
    node = args.device.resolve()
    print(f"Resolved device: {node}")
    try:
        stat = node.stat()
        print(f"Device identity: inode={stat.st_ino} "
              f"major={os.major(stat.st_rdev)} minor={os.minor(stat.st_rdev)}")
    except OSError as error:
        print(f"Device unavailable: {error}")
    interface = Path("/sys/class/video4linux", node.name, "device").resolve()
    print(f"Sysfs interface: {interface}")
    usb = next((p for p in (interface, *interface.parents)
                if (p / "idVendor").exists()), None)
    if usb:
        print(f"USB device: {usb}")
        for key in ("idVendor", "idProduct", "bcdDevice", "product", "speed",
                    "busnum", "devnum", "power/control",
                    "power/autosuspend_delay_ms", "power/runtime_status",
                    "power/runtime_active_time", "power/runtime_suspended_time"):
            try:
                print(f"{key}: {(usb / key).read_text().strip()}")
            except OSError as error:
                print(f"{key}: unavailable ({error})")
    command(["lsusb", "-t"])
    # Do not pass a missing stable path to tools that may interpret it loosely.
    if node.exists():
        command(["fuser", "-v", str(node)])
    print("\nRecent kernel USB/video events (current boot, last 30 minutes):")
    try:
        result = subprocess.run(
            ["journalctl", "-k", "-b", "--since=-30min", "--no-pager",
             "-o", "short-monotonic"], capture_output=True, text=True, timeout=10)
        lines = [line for line in result.stdout.splitlines()
                 if re.search(r"usb|uvc|xhci|video|suspend|resume", line, re.I)]
        print("\n".join(lines[-300:]))
        if result.stderr:
            print(result.stderr, end="")
        if result.returncode:
            print(f"journalctl exited {result.returncode}")
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"Kernel log unavailable: {error}")
    log_dir = Path.home() / ".var/app/com.obsproject.Studio/config/obs-studio/logs"
    logs = sorted(log_dir.glob("*.txt"), key=lambda p: p.stat().st_mtime)
    if logs:
        print(f"\nLatest Flatpak OBS log: {logs[-1]}")
        # Deliberately exclude other plugins, stream keys, and WebSocket config.
        lines = [line for line in logs[-1].read_text(errors="replace").splitlines()
                 if re.search(r"\[capture-card\]|v4l2-input:|v4l2-helpers:", line)]
        print("\n".join(lines[-300:]))
    else:
        print("No Flatpak OBS log found.")


if __name__ == "__main__":
    main()
