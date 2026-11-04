# Deadband on Windows (early)

Deadband runs on Windows 10 and 11, either as a standalone build (no Python
needed) or from source. It talks to the controller through Windows' own HID
driver, the same way GameSir Connect does. **Nothing is installed system-wide:
no driver, no Zadig/WinUSB swap, no service.** Firmware is never touched.

> **Status: not yet confirmed on hardware by the project.** Everything below
> has been tested in software against Windows' HID API shape (Windows Python,
> the real `hid.dll` calls, and a simulated Tarantula; see *What's been
> verified*). The register protocol itself was decoded from Windows USB
> captures of GameSir Connect, and is verified on a Tarantula Pro 8K under
> Linux. Until a Windows owner confirms their pad, configuration writes stay
> **locked per controller** until you approve them (see *First write*).

## Controllers

What works on Windows is what reaches the vendor HID channel (`0x0F` out,
`0x10`/`0x12` in), the same as on macOS:

| Controller | Windows |
|---|---|
| Tarantula Pro 8K / T3CE (`3537:103d`, PC mode) | live input, profiles, remaps, macros, sticks, triggers, gyro, poll rate, backup/restore |
| G7 Pro 8K PC, Cyclone 2 | same channel; expected to work, untested on Windows |
| G7 Pro, G7 SE, Kaleid | detected only: their settings use a raw-USB channel Deadband reaches on Linux only |
| Logitech G502 X | not yet tested on Windows |

**Tarantula in PC mode.** Configuration needs `3537:103d`. If the pad shows up
as `3537:103c`, GameSir's manual switches it to PC mode with **Home + X held for
2 seconds**. On Windows, 103d also shows up as an Xbox (XInput) controller to
games, and Deadband reads its live input from the vendor channel, so the two
coexist.

## Install the standalone build

1. Download `Deadband-windows-x64.zip` (or build it, below) and extract it
   anywhere, e.g. `C:\Users\<you>\Apps\Deadband`.
2. **Quit GameSir Connect** (including its tray icon). Both apps send commands
   on the same channel, and their commands would interleave.
3. Plug in the controller with a data cable and run `Deadband.exe`.

Windows SmartScreen may warn about an unsigned app the first time (*More info*
→ *Run anyway*). The folder holds two programs:

- `Deadband.exe` is the app. It has no console window; what it prints goes to
  `%LOCALAPPDATA%\deadband\deadband.log` (attach it to a bug report).
- `deadband-console.exe` is the same app with a console, for the diagnostics:
  `deadband-console.exe --doctor` (add `--probe`, with Deadband closed, for the
  read-only device questions).

To remove Deadband, delete the folder. Its data lives in
`%LOCALAPPDATA%\deadband\` (backups, recovery files, write approvals), and its
settings in the registry under `HKCU\Software\deadband`.

## Run from source

Install **Python 3.10 or newer** from python.org (tick *Add python.exe to
PATH*), then in PowerShell:

```powershell
git clone https://github.com/broroeror/gamesir-linux-tools.git deadband
cd deadband
py -m venv .venv
.venv\Scripts\pip install -r packaging\windows\requirements.txt
.venv\Scripts\python deadband.py
```

`run-deadband.bat` in `packaging\windows\` does the last step by double-click
once the venv exists.

## Build the .exe

```powershell
powershell -ExecutionPolicy Bypass -File packaging\windows\build.ps1 -Zip
```

The script creates a private build venv (`.venv-build`), installs the
dependencies plus PyInstaller and Pillow, runs the test suite, builds
`dist\Deadband\` from `packaging\windows\deadband.spec`, runs the built
`deadband-console.exe --doctor` as a self-check, and with `-Zip` writes
`dist\Deadband-windows-x64.zip`. The GitHub workflow
`.github/workflows/windows.yml` does the same on a Windows runner.

## First write: approve the controller

On Windows, Deadband starts every controller **read-only**: live input, the
profile indicator, and reading each page's settings all work, but nothing that
changes the controller is sent. A banner names the exact identity it found:

```
3537:103d "<product string>", firmware 2.49 (driven with the Tarantula Pro 8K register map)
```

Before approving, confirm it is your controller and check that the register
map fits it (next section). **Back up & enable writes → Yes, this is my
controller** then:

1. Backs up every profile to
   `%LOCALAPPDATA%\deadband\controller-backups\…_before-first-write.json`. To
   read profiles 2–4 the pad is switched through them (only the profile-select
   command is allowed during this step) and returned to the profile it was on.
2. Only if the backup succeeded, unlocks writes for **that identity**:
   vendor:product id, product string and firmware. A firmware update or a
   different pad asks again.

After that, Save works as on Linux: the pending bar writes, then reads every
change back to verify it. A recovery file of the old values is written first,
and the old values are put back if a write fails. **Backup / Restore → Restore**
writes any backup back. The approval list is
`%LOCALAPPDATA%\deadband\write-approvals.json`; delete an entry (or the file)
to lock a controller again.

(Linux and macOS skip this gate, since the project has verified them on
hardware. `DEADBAND_WRITE_APPROVAL=1` turns it on anywhere, `=0` turns it off.)

## Before approving: the read-only checks

With Deadband **closed**:

```powershell
.\deadband-console.exe --doctor --probe
```

(from source: `.venv\Scripts\python deadband.py --doctor --probe`). It only
asks questions; no setting is changed and no profile is switched. Check:

- **Devices**: your pad as `GameSir 103d "<product>" -> Tarantula Pro 8K`,
  with its HID collections listed. One of them (marked `*`) is the vendor
  collection, page `0xfff0`, with report ids `in [0x10 0x12]`, `out [0x0f]` and
  64-byte reports. That's the channel Deadband uses.
- **Probe: GameSir controllers**: replies to the firmware, active-profile and
  register-read questions.
- **Probe: register map check**: reads every analog field of the *active*
  profile with the Tarantula Pro 8K register map and judges whether each one
  decodes into range (deadzones 0–100 %, min ≤ max, a known poll-rate code).
  `VERDICT: every checked field decodes in range` means the map fits. If any
  field doesn't fit, **don't approve writes**. Open an issue with the output:
  a different model under the same USB id would need its own map.

The Tarantula **T3CE** shares the Pro 8K's USB id `3537:103d`. The project
hasn't confirmed that it shares the Pro 8K's register layout, which is exactly
what this check is for.

## Trying the UI without a controller

`python tools\sim_tarantula.py` opens the full app driven by a **simulated**
Tarantula (four HID collections shaped as Windows presents them, profile banks,
a live input stream). Approve, edit, save and back up freely: nothing reaches
real hardware. `python tools\sim_tarantula.py --selftest` runs the end-to-end
check headless. The app's own **Demo mode** (Settings) also works.

## Different from Linux

- **No permissions to set up.** There is no udev rule; Windows lets any desktop
  user open the vendor collection.
- **One app at a time.** Windows allows shared opens, so Deadband can't detect
  that GameSir Connect is also connected; `--doctor` warns when it sees GameSir
  processes running. Close them before configuring.
- **Press-to-select** (switching controllers by pressing a button) and the KDE
  **sticks → cursor** toggle are Linux-only; use the header's controller picker.
- **G7 Pro / G7 SE / Kaleid configuration** is Linux-only for now (they're
  detected and named).
- **Poll-rate changes re-enumerate the pad.** Windows may take a few seconds
  (sometimes a "setting up device" notice the first time at 2000 Hz+);
  Deadband reconnects by itself.

## What's been verified

| | How | Result |
|---|---|---|
| App starts, all pages render (Fusion style) | Windows Python 3.12 + PySide6 6.8 under Wine 9 | ✅ |
| Test suite (284 tests) | same | ✅ (5 Linux-only G7 SE tests skipped) |
| Windows collection discovery, vendor-collection pick, unit grouping | unit tests + simulator | ✅ |
| `hid.dll` capability queries (`HidP_GetCaps`, report ids) | real calls via Wine | ✅ calls; no device to query |
| Write gate, pre-approval backup walk, persisted approval | simulator, headless + clicking through the GUI | ✅ |
| Write + read-back verification | simulator | ✅ |
| Standalone `Deadband.exe` (PyInstaller) | built and launched under Wine | ✅ |
| **A real Tarantula on Windows 11** | — | ❌ not yet: needs the owner's `--doctor --probe` output, then an approved first write |
