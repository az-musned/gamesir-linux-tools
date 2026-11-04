"""What the host operating system can do, in one place.

Deadband started on Linux, where it leans on sysfs, evdev, udev and KWin; macOS
and Windows have none of those. Rather than test ``sys.platform`` wherever one
of them is used, callers ask this module about the *capability* they need
("can I read evdev?", "can I claim a raw USB interface?") and the per-OS answer
lives here. Adding or changing a platform is then an edit to this file, not a
hunt through the app.

Nothing here imports Qt or a HID library, so the diagnostics (which run before
Qt) and the tests can use it freely.
"""

import os
import subprocess
import sys

IS_LINUX = sys.platform.startswith('linux')
IS_MACOS = sys.platform == 'darwin'
IS_WINDOWS = sys.platform == 'win32'
NAME = 'linux' if IS_LINUX else 'macos' if IS_MACOS else 'windows' if IS_WINDOWS else sys.platform

# --- capabilities -------------------------------------------------------------

# /sys/class/hidraw, /sys/bus/usb: per-device USB topology, permissions, links.
HAS_SYSFS = IS_LINUX

# /dev/input/event*: the press-a-button-to-select loop and the G7 family's input.
HAS_EVDEV = IS_LINUX

# Claiming a vendor-class USB interface from the kernel driver (G7 Pro, G7 SE,
# Kaleid configuration). Located through sysfs, so Linux only.
RAW_USB_CONFIG = IS_LINUX

# Reaching a HID interface the OS left without a driver over libusb (a Cyclone
# dongle on macOS; see rawhid.py). NEVER on Windows: there every HID interface
# already has the in-box HID driver, and claiming one over libusb would need a
# WinUSB driver swap -- exactly the kind of system change Deadband must not make.
RAW_USB_HID_FALLBACK = IS_MACOS

# Device-file permissions a udev rule fixes (the access banner's "no-access").
HAS_DEVICE_PERMISSIONS = IS_LINUX

# On Windows, every top-level HID collection is its own device path, and the OS
# owns keyboard / mouse collections exclusively. Discovery offers only the
# vendor-defined collections (usage page 0xFF00 and up) as command channels.
PER_COLLECTION_HID_PATHS = IS_WINDOWS

# Windows support is new and has not yet been confirmed on hardware by the
# project, so configuration writes stay locked per controller identity until the
# user approves them (after an automatic backup) -- see write_gate.py.
# DEADBAND_WRITE_APPROVAL=1/0 forces it on/off on any platform (tests, or a
# Linux/macOS user who wants the same guard).
_approval_env = os.environ.get('DEADBAND_WRITE_APPROVAL', '').strip()
REQUIRES_WRITE_APPROVAL = (_approval_env == '1') if _approval_env in ('0', '1') else IS_WINDOWS


# --- where files go -------------------------------------------------------------

def user_data_dir():
    """Base directory for app data (recovery files, controller backups).

    Linux/macOS: $XDG_DATA_HOME or ~/.local/share -- unchanged from before the
    Windows port, so existing recovery files are still found. Windows:
    %LOCALAPPDATA% (C:\\Users\\<you>\\AppData\\Local). An explicitly set
    $XDG_DATA_HOME wins everywhere, so tests and tools can redirect it."""
    if os.environ.get('XDG_DATA_HOME'):
        return os.environ['XDG_DATA_HOME']
    if IS_WINDOWS:
        return os.environ.get('LOCALAPPDATA') or os.path.expanduser('~\\AppData\\Local')
    return os.path.expanduser('~/.local/share')


def user_config_dir():
    """Base directory for small config side-files. Linux/macOS: ~/.config (as
    before the port); Windows: %APPDATA% (roaming)."""
    if IS_WINDOWS:
        return os.environ.get('APPDATA') or os.path.expanduser('~\\AppData\\Roaming')
    return os.path.expanduser('~/.config')


# --- processes ----------------------------------------------------------------------

def no_window_flags():
    """`creationflags` for subprocess.run so a helper (git, tasklist) doesn't
    flash a console window from the windowed Windows build. 0 elsewhere."""
    return getattr(subprocess, 'CREATE_NO_WINDOW', 0) if IS_WINDOWS else 0


# Vendor configuration apps that talk to the same HID command channel. Two apps
# sending commands at once interleave on the pad, so the diagnostics name them.
VENDOR_APPS = ('gamesir',)            # GameSir Connect / GameSir Nexus executables


def running_vendor_apps():
    """Names of running processes that look like GameSir's own apps (Windows
    only; [] elsewhere or on any error). Read-only: lists, never touches them."""
    if not IS_WINDOWS:
        return []
    try:
        out = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True,
                             text=True, timeout=5, creationflags=no_window_flags()).stdout
    except Exception:
        return []
    found = []
    for line in out.splitlines():
        name = line.split('","', 1)[0].strip('"')
        low = name.lower()
        if any(low.startswith(v) for v in VENDOR_APPS) and name not in found:
            found.append(name)
    return found


def os_description():
    """Human OS name for reports: 'Windows 11 (build 26100)', 'macOS 15.1', ..."""
    import platform
    if IS_WINDOWS:
        ver = platform.version()                # '10.0.26100'
        try:
            build = int(ver.split('.')[2])
        except (IndexError, ValueError):
            build = 0
        # Windows 11 still reports major version 10; the build number tells them apart.
        name = 'Windows 11' if build >= 22000 else f'Windows {platform.release()}'
        return f'{name} (build {build})' if build else name
    if IS_MACOS:
        return f'macOS {platform.mac_ver()[0]}'
    return f'{platform.system()} {platform.release()}'
