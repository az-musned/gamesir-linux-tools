# gsprobe — read-only input timing probe (Windows)

Diagnostics for the GameSir Tarantula (`3537:103D`) ahead of a low-latency
ViGEm/HidHide remapper. It reads input and changes nothing. It never writes
HID output or feature reports, sends vendor commands, hides devices, creates
virtual devices, or touches reWASD or GameSir Connect.

| File | What it is |
|---|---|
| `gsprobe.cpp` | The probe (one file, C++17, Win32 only) |
| `build.bat` | Builds `gsprobe.exe` with MSVC or MinGW-w64 |
| `inspect.ps1` | Read-only snapshot: processes, services, PnP devices with service/filters/parent/driver, class filters, HidHide registry, driver store, reWASD config listing |
| `run-tests.ps1` | Guided session: runs everything below in three scenarios and saves to `results\` |

## Commands

```
gsprobe enum [--all]       HID collections + XUSB interfaces + XInput slots, each with its driver stack
gsprobe hid    [opts]      event-driven HID report timing (overlapped ReadFile, 4 reads outstanding)
gsprobe xinput [opts]      XInputGetStateEx polling on a high-resolution waitable timer
gsprobe both   [opts]      both at once, plus press-to-press correlation between the two streams
  --dev N  --slot N  --period-us U  --spin  -t S  --csv FILE  --show  --high  --all
```

## What each test answers

| Question | Test | What to read |
|---|---|---|
| HID and XInput interfaces (Phase 1.1) | `enum --all` | Each collection's usage, report IDs, `mi`/`ig`/`col`, and its stack (service, device and class filters) up to the USB device |
| Standard path vs vendor telemetry (1.4) | `enum` | Role column: `XInput HID companion (IG_)`, `game controller`, `vendor-defined` (interface `mi=01`) |
| Event-driven stream for normal input? (1.3) | `hid` on the standard collection | Reports per second while sticks move. The decoded button and axis changes show whether all controls are present |
| Report rate, gaps, irregular timing (1.2, 1.7) | `hid -t 30` with sticks moving; `hid -t 10` idle | Arrival-interval histogram, ">1.5× median" (late/missed) and "<0.25× median" (batched) counts. Idle run: does it report when nothing changes? |
| Vendor stream without GameSir Connect (1.5) | `hid --dev <vendor>` in S0 vs S1 | Reports per second with Connect closed vs open. Passive only: the probe sends no heartbeat |
| Shared read-only access (1.6) | `enum` | `shared-read open: ok` / `FAILED (err 5 access denied / err 2 hidden)` |
| XInput polling cost and jitter (Phase 4 B) | `xinput --period-us 1000` and `250` | Poll-tick histogram (timer jitter), `XInputGetState` call cost, packet-change rate, packet numbers skipped between polls, process CPU % |
| Can HID wake a hybrid reader? (Phase 4 C) | `both --period-us 250` | `XInput minus HID` delta for the same presses. A consistently positive floor means HID arrives first |
| reWASD: virtual pad? physical hidden? (Phase 2) | S2 `inspect` + `enum --all` | An extra XInput slot or XUSB interface (`045e:028e` or other), whether the GameSir is still listed or openable, and reWASD services and filters in the stack |
| reWASD processing delay (Phase 2) | S2 `both --dev <physical> --slot <virtual>` | Physical HID arrival → virtual XInput visibility, plus ≤ one poll period. Works only if reWASD leaves the physical HID readable |
| reWASD macro timeline (baseline) | S2 `xinput --slot <virtual> --csv` | Step-to-step timing of reWASD's own macro output |

## What this can and cannot measure

**It measures:**
- user-mode arrival times
- intervals and jitter between arrivals
- the relative order of two user-mode paths (HID vs XInput)
- a remapper's physical-in → virtual-out delay, if both ends are readable
- the probe's own CPU cost

All timestamps come from QPC; resolution is printed at start.

**It cannot measure:**
- time inside the controller: button scan or ADC, firmware filtering
- USB bus time
- kernel time before the report reaches user mode
- when Rocket League samples input (its 120 Hz physics tick)
- render and display time

A poll or report interval is **not** input-to-game latency. To get that, you
need external timing: a ≥240 fps camera filming the pad and the screen, or a
photodiode on the screen with a switch wired in parallel to a button. Use the
same game action in each configuration.

## Interpreting results for the remapper design

The data decides between:

* **A — HID-only, event-driven + ViGEm.** Needs a standard HID collection
  whose reports carry *every* control at full fidelity. For an XInput-mode pad,
  the HID companion merges LT and RT onto one axis, so A is expected to fail
  that test. `enum`/`hid` confirm it either way.
* **B — XInput polling + ViGEm.** Always works. Average sampling delay is
  period/2. The cost and jitter come from the `xinput` runs.
* **C — Hybrid.** A HID report wakes the reader and `XInputGetStateEx` reads
  the full state. This needs a HID stream that reports continuously while
  input changes, plus the XUSB state already updated when the HID wake
  arrives. `both` shows the second: if XInput lags HID by more than a few tens
  of µs beyond sampling, a wake-then-read would see stale state and C needs a
  short re-poll.
