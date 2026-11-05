# gsmacro: Windows setup guide

**Status: built and tested offline. The real controller, ViGEmBus and HidHide
path has not been validated on hardware yet.** Use the
[hardware checklist](HARDWARE-CHECKLIST.md) for the first session.

## What you need

| Item | Notes |
|---|---|
| Windows 10 or 11, x64 | No Visual Studio, .NET or VC++ runtime needed. Everything is statically linked. |
| GameSir Tarantula, wired, PC/XInput mode | USB ID `3537:103D`. Hold Home + X for 2 s if it shows up differently. |
| ViGEmBus driver | Already installed on your PC. gsmacro only talks to it. |
| HidHide driver | Already installed. gsmacro only reads its status. You configure hiding yourself in HidHide's own client. |
| `ViGEmClient.dll` | Included in the ZIP, built from the official source (see `licenses/`). |

gsmacro **never** installs or uninstalls drivers, never changes HidHide,
reWASD or GameSir Connect, and never touches the controller's firmware or
profiles.

## Install

1. Extract the ZIP to a normal folder, e.g. `C:\Tools\gsmacro\`. Don't use
   Program Files.
2. Keep `ViGEmClient.dll` next to `gsmacro.exe`.
3. The exe is unsigned, so Windows SmartScreen may warn on first launch:
   *More info → Run anyway*. You can compare the files against
   `SHA256SUMS.txt` first.
4. Settings are saved to `%APPDATA%\gsmacro\gsmacro.cfg`. Delete that folder
   to uninstall completely; nothing else is written.

## First run (guided)

On first start gsmacro opens **Setup & validation** (also under **Setup...**
in the top bar). Work through the steps in order:

1. **Controller detected.** It must list the GameSir (VID 3537) in an XInput
   slot. Unapply reWASD first: reWASD can hide the physical pad.
2. **ViGEmBus and ViGEmClient.dll.** Opens a bus connection without
   creating a device. gsmacro keeps that one connection until it exits
   (closing it early can hang ViGEmClient), so this is normal.
3. **Start Live Mode.** Creates one virtual Xbox 360 pad. **Macros start
   OFF** (pure passthrough).
4. **Check passthrough.** Open `joy.cpl` and test every button, stick and
   trigger on *Controller (XBOX 360 For Windows)*. The Live self-test
   compares the virtual pad (read back through XInput) with gsmacro's output.
   Tick the box when everything behaves normally.
5. **HidHide available.**
6. **Hide the physical pad (manual).** In HidHide Configuration Client:
   * Applications tab: add `gsmacro.exe`.
   * Devices tab: tick the GameSir entries, then tick *Enable device hiding*.
   * `joy.cpl` should now list exactly one controller.
7. **Arm macros.** Test each macro with **Test** and with L1/R1 + Square while
   watching the output timeline.
8. **Turning it off.** Click **Demo** or **Stop Live Mode**, or close
   gsmacro: the virtual pad disappears with every input released.

## Undo everything / restore normal controller use

* Stop Live Mode or close gsmacro. The virtual pad is removed immediately.
* In HidHide, untick the GameSir (or *Enable device hiding*). Games see the
  physical pad again. Replug it if a running game doesn't notice.
* Re-apply your reWASD config if you want to go back to reWASD.

## Your macros

The built-in macros are **examples only**. Enter your real wall-dash steps
in one of these ways:

* edit them in the step table
* drag the wait handles on the macro timeline (hold Shift to snap to 120 Hz
  ticks)
* paste them in **Edit as text**
* use **Import...** on a text file

Then **Save**. `examples\` holds more sample configs: passthrough-only,
zero-gap FIFO and a tick-aligned template.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| "GameSir NOT found" | Cable, PC mode (Home + X, 2 s), reWASD still applied, or HidHide hides it from gsmacro: add `gsmacro.exe` in HidHide's Applications tab. |
| "ViGEmClient.dll not found" | Keep the DLL next to the exe; re-extract the ZIP. |
| `vigem_connect failed: 0xE0000001` | ViGEmBus driver not running. Check Device Manager → System devices. gsmacro won't install it. |
| Game reacts twice / sees two pads | The physical pad isn't hidden (setup step 6). |
| "Virtual pad updates are failing" | ViGEmBus stopped. Click Demo, run the setup checks again. |
| Test button says a controller is needed | gsmacro cancels macros while no controller is connected (so nothing can stay pressed). |
