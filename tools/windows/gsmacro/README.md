# gsmacro — low-latency controller macros for the GameSir Tarantula

A small Windows tool that keeps normal controller input as direct as possible
and runs **controller-native** Rocket League macros. It never emulates a
keyboard or mouse.

* **L1 / R1**: air roll, passed through untouched.
* **Square** alone: passed through in the same sample. Nothing waits to see
  whether a bumper follows.
* **L1 held + Square**: left macro. **R1 held + Square**: right macro. Square
  is withheld from the game for that press; the bumper stays held.
* Macros can use button press/release/suppress, analog stick and trigger
  values, and fractional-millisecond waits. Activations run **FIFO**, never
  overlapping, with a configurable neutral gap between them.

> **Status: offline build. The real controller has not been tested.**
> Everything below "Verified" was built and run in a cloud container with
> mock input. The Windows XInput / ViGEmBus / HidHide pieces compile and are
> wired in, but they have **not** been run with the controller, ViGEmBus or
> HidHide. See [Hardware-dependent tasks](#hardware-dependent-tasks-remaining).

![Left macro running](docs/screenshots/02-left-macro-running.png)

## Architecture

```
 physical pad ─▶ IInputSource ─┐                       ┌─▶ IOutputSink ─▶ game
 (XInput / mock)               │  Engine thread        │   (ViGEm X360 / mock)
                               ▼                       │
          wait(min(next macro event, next poll | input event))
                               │
              read ─▶ Processor ─▶ submit only if changed
                       │ chord detector (L1/R1 + Square edges)
                       │ MacroScheduler (FIFO, absolute deadlines, overrides)
                       └ merge: (phys & ~withheld & ~suppressed) | forced; axis overrides
 GUI thread ◀── snapshot (try-lock: engine never waits) + lock-free timeline ring
```

| Layer | Files | Notes |
|---|---|---|
| Core (portable, no OS calls) | `core/` | `Processor` is pure and allocation-free per sample. `MacroScheduler` uses a fixed 8-slot ring, absolute event times and drift-free catch-up. `Engine` runs one thread with try-lock publishing. |
| Mock / demo | `backends/mock_backend.h`, `backends/demo_scenarios.*` | Event-driven or polled mock pad, recording sink, 8 scripted scenarios |
| Windows | `backends/win/` | `XInputSource` (selects the pad by VID 3537, so it can never read back its own virtual pad), `ViGEmSink` (loads `ViGEmClient.dll` at run time), `WinWaiter` (high-resolution waitable timer + MMCSS "Games"), `hidhide_status` (read-only) |
| GUI | `app/` | Dear ImGui 1.91.9 (vendored, MIT). Win32 + DX11 host on Windows; GLFW + OpenGL host for development and screenshots |

### Latency design decisions

* **Square has no added delay.** A chord needs the bumper to be held already
  (or pressed in the same sample). Allowing "Square then bumper" would mean
  holding back every Square press, and that latency would hit normal play.
* **Macros start in the same sample as the chord.** Chord detection runs
  before the scheduler update, so the first macro step is in that sample's
  output.
* **Output is submitted only on change.** An idle controller costs no
  virtual-pad updates.
* **Waits are scheduled against absolute deadlines.** Each event fires at
  `start + offset`. A late wake applies every due event, and the next event
  stays on schedule, so errors never accumulate. PadForge resets a step's
  timer when it notices the step is done, so its errors add up.
* **Spinning is limited to macro deadlines.** The final `spin tail` µs before
  a macro event are spun for precision. Poll and idle wakes never spin.
* **Gap between queued macros (default 10 ms).** That is more than one
  120 Hz game tick, so the game sees a release before the next macro presses
  the same button again.
* **The engine never waits for the GUI.** The GUI reads snapshots published
  with try-lock, so a slow GUI frame can't stall input.

## Build

Requirements: CMake ≥ 3.16 and a C++17 compiler. There are no other
dependencies (Dear ImGui is vendored).

**Windows (Visual Studio 2022, "Desktop development with C++"):**

```bat
cmake -S tools\windows\gsmacro -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\gsm_tests.exe
build\Release\gsm_bench.exe
build\Release\gsmacro.exe
```

**Windows with MinGW-w64, or cross-compiling from Linux:**

```sh
cmake -S tools/windows/gsmacro -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=<mingw toolchain file>
ninja -C build-win      # gsmacro.exe, gsm_tests.exe, gsm_bench.exe (statically linked)
```

**Linux development build** (Demo Mode only; needs `libglfw3-dev` and `libgl-dev` for the GUI):

```sh
cmake -S tools/windows/gsmacro -B build -G Ninja && ninja -C build
./build/gsm_tests
./build/gsm_bench
./build/gsmacro                         # interactive demo
xvfb-run ./build/gsmacro --screenshots DIR   # scripted screenshots (PPM)
```

**Live mode needs `ViGEmClient.dll`** next to `gsmacro.exe`. Build it from
[nefarius/ViGEmClient](https://github.com/nefarius/ViGEmClient) (MIT) as a
x64 DLL. It's only a user-mode library; the ViGEmBus driver it talks to is
already installed. Without the DLL, the app still runs in Demo Mode.

## Using it

* **Demo Mode** starts automatically, with no hardware, drivers or virtual
  device involved.
  * Click L1 or R1 to latch them, then click Square.
  * Drag a stick to move it.
  * Or press **Play** on a scenario: Square alone, L1+Square, R1+Square, five
    rapid presses (FIFO), steering during a macro, bumper after Square, both
    bumpers, disconnect mid-macro.
  * The **virtual output** pad and the **timeline** show what the game would
    receive, on a 120 Hz game-tick grid. Blue is passthrough, orange is
    macro-driven, and red outline is withheld.
* **Macros:** edit steps in the table (action, buttons, axis value, wait in
  ms, with each wait's length in game ticks), or switch on **Edit as text**.
  Warnings flag steps the game can never see, such as a press released in the
  same instant.
* **Synchronization:** poll period, spin tail, FIFO capacity, gap between
  macros, L1+R1+Square policy, chord/modifier buttons, and game tick rate.
* **Config file:** `%APPDATA%\gsmacro\gsmacro.cfg` on Windows. Use the
  Save/Load buttons.

```
[settings]
poll_period_us = 500
spin_tail_us = 300
queue_capacity = 2
inter_macro_gap_ms = 10
both_bumpers = latest        # latest | left | right | ignore

[macro left]
name = Wall dash left
axis LX -1
press Cross
wait 25
release Cross
wait 25
axis-release LX
wait 17
```

The two built-in macros are **templates** that show the format. They are not
tuned wall-dash inputs. Transcribe your working reWASD macro timings into
them.

## Verification done offline

| Check | Result |
|---|---|
| Unit and integration tests (`gsm_tests`): chord detection, timing, FIFO, merging, cancellation, disconnect, cleanup, config and text parsing, engine threading | **42/42 pass** (Linux, 5 consecutive runs) |
| Same tests under ThreadSanitizer and AddressSanitizer/UBSan | 42/42 pass, **0 sanitizer reports** |
| Windows build (MinGW-w64 cross-compile): GUI, tests, bench | Builds with no warnings in project code; imports only system DLLs |
| Windows test binary run under Wine 9.0 | **42/42 pass** |
| Windows GUI binary under Wine (DX11 via wined3d) | Starts and renders; see `docs/screenshots/win-build-under-wine.png` |
| GUI screenshots (Linux GLFW build under Xvfb) | `docs/screenshots/` |

### Numbers measured offline (cloud container, mock backend, not your PC)

From `gsm_bench` on a 4-vCPU shared cloud VM (Xeon 2.1 GHz, Linux, no
real-time tuning). They show how cheap the software path is and how timing
behaves on that VM. They are **not** Windows figures and **not**
input-to-game latency, because no controller, USB, driver or game is
involved. Run `gsm_bench.exe` on your PC for figures that mean something
there.

| Measurement | Result on the cloud VM |
|---|---|
| Engine cycle cost (read + chord + scheduler + merge + submit) | mean ≈ 2.2 µs (GUI diagnostics panel) |
| Mock input change → sink submit, event-driven wake (n = 2000) | mean 74.9 µs, p50 ≤ 100 µs, p99 ≤ 500 µs, max 2.19 ms |
| Macro event lateness, spin tail 0 / 100 / 300 µs (n = 400 each) | mean 176 / 70 / 30 µs; p50 ≤ 200 / 50 / 2 µs; max 7.5 / 2.4 / 2.5 ms |
| Engine-thread CPU while running macros back to back, spin tail 0 / 300 µs | 0.46 % / 1.82 % of one core |
| Engine-thread CPU idle, event-driven source | 0.14 % of one core |
| Engine-thread CPU idle, polled every 500 µs | 4.7 % of one core |

The millisecond maxima come from scheduling noise on the shared VM. Whether
Windows on your PC does better (high-resolution timer + MMCSS) is
hardware-dependent task 4 below. Under Wine the same binary showed ~1 ms
timer granularity, a known Wine limitation that says nothing about Windows.

## Hardware-dependent tasks remaining

None of these can be done or claimed without your PC:

1. **Input path.** Run `tools/windows/gsprobe` (`run-tests.ps1`). It confirms
   the XInput slot and VID detection, the real report rate, and whether a
   HID-wake hybrid beats polling. Only then add an event-driven source behind
   `IInputSource`, or tune `poll_period_us`.
2. **ViGEm output.** Build `ViGEmClient.dll`, start Live mode, and confirm one
   virtual Xbox 360 pad appears and follows the controller. Check the
   `XUSB_REPORT` and notification signatures against the DLL you build.
3. **HidHide setup (manual, in HidHide's own client).** Hide `3537:103D`, add
   `gsmacro.exe` to the allowed apps, and confirm Rocket League sees exactly
   one controller. gsmacro only reads HidHide's status.
4. **Timing on Windows.** Run `gsm_bench.exe` and the GUI diagnostics.
   Choose `spin_tail_us` from the measured event lateness against CPU cost.
5. **Rumble pass-through.** Game → ViGEm notification → `XInputSetState` on
   the physical pad. The code is wired in but untested.
6. **Disconnect and replug** with the real pad, and with Live mode running
   (the engine outputs neutral and rescans once a second).
7. **reWASD baseline and macro transcription.** Copy your reWASD macro steps
   into the left/right macros. Compare timelines against
   `gsprobe xinput --csv` captures of reWASD's output.
8. **End-to-end latency.** Compare reWASD, gsmacro and the bare controller
   with a ≥240 fps camera in Rocket League free play. Software timings can't
   stand in for this.

## Known limitations

* Input is read by polling XInput for now. An event-driven HID-wake variant
  waits on the gsprobe data (task 1).
* ViGEmBus is retired upstream but still works on Windows 11. It's already
  installed on the target PC, and no other driver is used.
* "Square, then bumper" never triggers, by design (see the latency design
  decisions above).
* Physical-pad hiding can't be verified from inside the app. The Live panel
  requires you to confirm the HidHide and reWASD checklist before it creates
  the virtual pad.
