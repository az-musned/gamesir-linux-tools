# Low-latency controller macros on Windows — investigation

Scope: GameSir Tarantula (USB `3537:103D`, wired, firmware 2.4.9) on Windows 11,
Rocket League, existing HidHide + ViGEmBus. Goal: replace reWASD with a free tool
that keeps L1/R1 as air roll, fires a left/right wall-dash macro on
L1+Square / R1+Square, runs macros FIFO, adds as little latency as possible to
normal input, and outputs controller input only (no keyboard or mouse).

Investigation only. Nothing was installed, changed or injected.

## What this report rests on

| Evidence | Source | Kind |
|---|---|---|
| PadForge architecture | Source at `hifihedgehog/PadForge` @ `7b96ac7` (2026-11-02), read in full where cited | Code-derived |
| Tarantula `3537:103d` interfaces, firmware macro format | This repo: `controller_profile.py` (Tarantula profile), `docs/RESEARCH.md` | Measured on hardware by this project (on a Tarantula **Pro 8K**; see note) |
| Rocket League physics rate (120 Hz) | Psyonix, GDC 2018 "It IS Rocket Science!" | Public |
| HIDMaestro submit time | PadForge README (vendor claim) | **Unverified** |
| Your reWASD setup | Not inspected (see §4) | — |
| End-to-end latency numbers | **None measured.** No Windows machine or controller is reachable from this session | — |

Every latency figure below is either derived from code or labelled as something
to measure. Nothing here is an invented measurement.

**Identity note.** You call the pad a Tarantula CE / T3CE. This repo knows
`3537:103d` as the Tarantula Pro 8K in PC/XBOX mode: XInput on interface 0 and
a GameSir vendor HID channel on interface 1. The VID:PID matches. Firmware
details in §3F come from the Pro 8K and may differ on yours.

---

## 1. PadForge analysis

| Question | Finding | Where |
|---|---|---|
| How is physical input captured? | **Polling** through SDL3. `SDL_UpdateJoysticks()` runs once per cycle, then each device's `GetCurrentState()` is read. It does not react to input events. | `InputManager.cs:2209`, `Step2.UpdateInputStates.cs:111` |
| Polling frequency / scheduling | One `AboveNormal` thread runs a fixed-period loop. The default `PollingIntervalMs = 1` (1000 Hz, adjustable 1–16 ms). It waits on a `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer, spins for the last 0.1 ms, and corrects drift against the wall clock. Device enumeration runs inside the same loop every 2 s. | `InputManager.cs:38, 98, 2026–2370` |
| How are normal inputs forwarded? | **Every** input goes through the whole pipeline each tick: Step 2 read → Step 3 mapping → Step 4 combine → Step 4b macros → motion/DSU → Step 5 virtual output. There is no bypass path. | `InputManager.cs:2239–2265` |
| Virtual output | **HIDMaestro**, PadForge's own UMDF2 virtual-HID driver (337 profiles). For composite USB profiles it also installs usbip-win2's virtual USB host controller. **v3 does not use ViGEmBus.** Its first-run "legacy cleanup" offers to **uninstall ViGEmBus**. | README §HIDMaestro; `Step5.VirtualDevices.cs:2870–2892`; `DriverInstaller.cs:204–230`; `MainWindow.xaml.cs:2923` |
| Are physical controllers hidden? | Yes, through HidHide, which PadForge drives (HID, XUSB and XboxComposite classes). | `HidHideController.cs:100–107` |
| Controller-native analog macros? | Yes. `AxisSet`, `AxisHold`, `AxisAdd` and `AxisSetLatched` act on sticks and triggers, and `ButtonPress` / `ButtonRelease` on buttons. Macros can *yield* to the physical stick (`AxisYieldToPhysical`). | `Step4b.EvaluateMacros.cs:2995–3030, 4283+, 4449+` |
| Chord trigger (L1+Square) | Supported (multi-button trigger). But `ConsumeTriggerButtons` strips **all** trigger buttons, so L1 (air roll) would also drop out during the macro. To avoid that, make L1 a shift layer and put the macro on Square inside it. | `Step4b.EvaluateMacros.cs:955–976` |
| Macro timing model | Macros are evaluated **once per poll tick**. Only **one sequential action runs per tick**: even instant actions like `ButtonRelease` or `AxisSet` use up a tick. When a step ends, `ActionStartTime = DateTime.UtcNow`, so each step starts from the moment it was noticed rather than from its scheduled deadline. Each step can run up to one tick long, and **these errors add up** over the sequence. | `Step4b.EvaluateMacros.cs:2443–2455, 2934–3010, 4274–4280` |
| Rapid re-activation | `shouldStart && !macro.IsExecuting`: a press during a running macro is **dropped**, not queued. There is no FIFO. | `Step4b.EvaluateMacros.cs:825` |
| Can FIFO be added easily? | Adding a per-macro pending counter is small. Doing it correctly inside the 7.2k-line macro evaluator, with its pair, latch, combo-break and layer rules, is not. | — |
| Can it be made lower-latency? | Only in part. The sampling loop, the all-in-one pipeline and the HIDMaestro dependency are structural. The codebase is ~674k lines of C# (InputManager partials alone are ~36.6k) and changes daily upstream, so a fork is expensive to maintain. | — |

## 2. Can normal input stay direct while only macros pass through software?

**Not with user-mode software.** These are the Windows input paths:

* **XInput is poll-only and has no write path into a physical device.** The
  game's `xinput1_4.dll` sends a state request (IOCTL) to the pad's `xusb22`
  device, which returns the controller's last report. User-mode software
  cannot add data to that physical stack.
* **"Merging" through a second controller does not work.** If the physical pad
  stays visible and a virtual pad sends only macros, the game sees two devices.
  It reads player 1 from one of them. It never combines two pads' stick values,
  so a macro's stick cannot override the physical stick.
* **`Windows.UI.Input.Preview.Injection` (`InjectGamepadInput`)** is the only
  documented gamepad injection API. It needs a restricted capability, targets
  Windows.Gaming.Input consumers, and creates its *own* virtual gamepad. That
  makes it another second device, not a merge. Not viable.
* **Hooking XInput inside Rocket League** would work technically, but it means
  injecting into the game, which is out of scope.
* **A kernel filter driver on the pad's own stack** is the only way to keep the
  physical device as the one the game reads while splicing macro state into it.
  An upper filter on the XUSB device would rewrite `IOCTL_XUSB_GET_STATE`
  completions with a macro overlay supplied by a user-mode service. Normal
  input then gains only a memcpy in the completion routine, and the overlay is
  sampled exactly when the game polls. I believe this is how reWASD's "no
  virtual controller" remapping works (unverified; §4).
  Requirements:
  * a KMDF driver (no maintained open-source one does this)
  * attestation signing (EV certificate + Microsoft Partner Center), or
    test-signing mode, which anti-cheat may object to
  * accepting the risk of a BSOD
  * installing a driver

  That breaks the "no new drivers" and "prefer open source" constraints.

**The closest feasible alternative:** hide the physical pad with HidHide, then
use a minimal user-mode passthrough that wakes on input events. It forwards
each new report to one ViGEm Xbox 360 pad as soon as the report arrives, and
lays macro state over it. The game sees exactly one controller. Done well, the
added latency is the passthrough's own processing time plus the ViGEm update.
There is no fixed-rate sampling delay.

How to make the passthrough event-driven when XInput only polls:

1. Wired XUSB pads also expose an XInput-compatible HID collection (device path
   contains `IG_`; SDL's RAWINPUT backend relies on this). Register for Raw
   Input on that collection. Each `WM_INPUT` is a "new report" signal, and an
   immediate `XInputGetState` then reads the full state. The HID view alone
   combines both triggers on one axis, so XInput is still needed for full
   fidelity. Whether the HID arrival comes before or after the XUSB state update
   **has to be measured** (§5).
2. Fallback: busy-poll `XInputGetState` and watch `dwPacketNumber` on a
   dedicated core. Polling every 0.25 ms bounds sampling delay at 0.25 ms, at
   the cost of one busy core.
3. Not recommended: the GameSir vendor `0x12` stream (interface 1). It needs a
   heartbeat write every 0.5 s (`reader.py:81–103`), and GameSir Connect may
   compete for it.

Every approach needs `XInputGetStateEx` (ordinal 100) to read the Guide button.

## 3. Approaches compared

Notation: *added* = latency added on top of the controller → `xusb22` → game
path that every approach shares. Rocket League samples input on its **120 Hz
physics tick (8.33 ms)**, which dominates everything else and is identical
across approaches.

Why macro jitter still matters at that tick rate: a step edge that lands at a
random point relative to the tick grid moves to a different tick with
probability ≈ *error* / 8.33 ms. A 1 ms timing error moves ~12% of edges by a
whole physics tick. A 0.1 ms error moves ~1.2%.

| | A. PadForge as-is | B. PadForge fork | C. Macro-only app, physical pad direct | D. Custom passthrough (ViGEm) | E. Kernel filter driver | F. Pad firmware macros |
|---|---|---|---|---|---|---|
| Normal-input added latency | 0–1 ms sampling delay (mean ~0.5) at 1 ms poll, plus per-tick pipeline compute (**unmeasured**), plus HIDMaestro submit (vendor claim 48 µs for DualSense; Xbox **unmeasured**) | Same structure. Faster polling shrinks the sampling term but is never zero | 0 | Event-driven: processing + ViGEm update (**to measure**; expected µs-scale). Busy-poll fallback: + ≤0.25 ms | ≈0 (in-line) | 0 |
| Macro activation latency | 0–1 ms (next tick) | 0–1 ms, or less with faster polling | n/a | Processing on the same input event (**to measure**) | Next game poll | In-pad |
| Macro timing jitter | Each step rounded **up** to a tick, **errors add up**; instant steps cost a full tick | Fixable (absolute deadlines) | n/a | Absolute QPC deadlines + high-resolution timer + final spin: expected sub-0.1 ms (**to measure**) | Same as D | ms fields; real jitter unknown |
| FIFO | No (drops) | Addable, with effort | n/a | Designed in | Designed in | No |
| Reliability | Mature, many features = many moving parts | Fork drift | **Broken**: two controllers, no merge | Small surface; depends on ViGEmBus (retired upstream, still works; already installed) | BSOD / signing / anti-cheat risk | Very high |
| CPU | One AboveNormal thread at 1 kHz plus the full feature pipeline | Same | Low | Low, if event-driven | Negligible | None |
| Complexity | Configuration only | High (huge codebase) | Low | Moderate: ~1–2k lines of C++ | Very high | Configuration only |
| Rocket League compatibility | Virtual X360 via HIDMaestro: expected OK | Same | Poor | Virtual X360 via ViGEm: the DS4Windows/x360ce-style path RL players use | Physical pad unchanged | Physical pad unchanged |
| Constraints | **Installs HIDMaestro + usbip-win2; offers to remove ViGEmBus** | Same | — | Uses only drivers you already have | **Custom driver** | Needs a GameSir Connect change later |
| Main limitation | Polled pipeline; no FIFO; drift | Maintenance | Not viable | ViGEm is end-of-life | Out of constraints | Triggered only by programmable buttons (L4/R4/C1–C4/T1–T3), **not** L1+Square. Stick steps are full-deflection directions only, with no magnitude (`docs/RESEARCH.md`, 8K findings) |

**F is a real option if you accept a back button instead of the chord.** The
firmware runs the macro inside the pad, merges it natively and adds zero host
latency. It can't do L1+Square, partial stick deflection or FIFO. The `e7`
"Shift layer" target and the unknown fifth bank might allow a chord. Both are
unexplored.

## 4. Your reWASD setup (not inspected)

No folder from your PC is shared with this cloud session, so I could not read
your reWASD configuration. Nothing was touched. These read-only PowerShell
commands (no admin needed except where marked) answer "virtual controller or
in-stack remap?". Run them with your config **applied**:

```powershell
# 1. Which controllers exist right now? A "reWASD"/"Virtual"/"Xbox 360" device
#    beside the GameSir means a virtual controller is in use.
Get-PnpDevice -PresentOnly |
  Where-Object { $_.InstanceId -match 'VID_3537' -or $_.FriendlyName -match 'reWASD|Virtual|Xbox|ViGEm' } |
  Format-Table Status, Class, FriendlyName, InstanceId -AutoSize

# 2. Is a reWASD filter in the GameSir's own driver stack? (paste the InstanceId)
$id = '<GameSir InstanceId from step 1>'
Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_Service, DEVPKEY_Device_UpperFilters, DEVPKEY_Device_LowerFilters |
  Format-Table KeyName, Data -AutoSize

# 3. Installed input drivers (admin)
pnputil /enum-drivers | Select-String -Pattern 'rewasd|vigem|hidhide' -Context 2,4

# 4. Where the configs live (read only; do not open them in reWASD's editor)
Get-ChildItem "$env:PUBLIC\Documents\reWASD", "$env:ProgramData\reWASD" -Recurse -Filter *.rewasd -ErrorAction SilentlyContinue
```

How to read the results: *virtual pad present* → reWASD works like approach D
(using its own bus driver), so D is a like-for-like replacement. *No virtual
pad, reWASD filter on the GameSir stack* → reWASD works like approach E, and D
could add a little latency by comparison. §5 measures how much.

## 5. Benchmarking method

Every tool here is read-only. Steps 1–3 need nothing but software. Step 4 gives
the only true game-visible figure.

1. **Physical detection** (`probe.exe`, ~200 lines of C++, to be written in
   phase 1). On a time-critical MMCSS thread, record a QPC timestamp for
   (a) each `WM_INPUT` from the pad's `IG_` collection and (b) each change of
   `XInputGetStateEx` `dwPacketNumber`, from a busy-poll. The output is the
   true report rate, the inter-report jitter, and whether the Raw Input signal
   arrives before or after the XUSB state update. This decides between the
   event-driven and busy-poll designs.
2. **Processing overhead.** Inside the passthrough, record QPC at input
   arrival and after `vigem_target_x360_update` returns. Report
   p50/p99/p99.9/max over ≥100k reports while you play normally.
3. **Virtual-controller update latency.** A second process busy-polls the
   *virtual* pad (`dwPacketNumber`) and shares QPC timestamps with the
   passthrough through shared memory, giving physical-arrival → virtual-visible
   latency. Running the same probe on scripted macros gives per-step timing
   error against the intended timeline. This works for reWASD's macros too, if
   it presents a virtual pad, which makes a direct jitter baseline.
4. **Game-visible timing.** Film the controller and the monitor together with a
   high-speed camera (≥240 fps; 960 fps phone modes exist) in Rocket League
   free play, using one fixed action such as jump. Run ≥50 trials each on:
   physical pad only (no software), reWASD, and the candidate. The difference
   between configurations is the added latency at frame resolution. Also run
   **PresentMon** (open source) in each configuration to compare frame-time
   consistency, since frame drops were your original complaint.

Rules: same USB port, same poll rate, nothing else running, and report
distributions rather than averages. For steps 1–3, quit reWASD first, without
changing its settings, because it hides the device. Do that yourself; this
session can't reach your PC.

---

## Answers

1. **Can normal input stay completely direct with only macros in software?**
   Not in user mode. It takes a kernel filter driver on the pad's stack
   (approach E), which is what a custom signed driver would have to do.
   Hooking XInput inside the game would also work, but that is excluded.
2. **Closest feasible alternative:** HidHide plus a minimal **event-driven**
   passthrough to one ViGEm Xbox 360 pad, with a macro overlay. There is one
   visible controller and no fixed sampling delay. The cost is processing time
   plus the ViGEm update, still to be measured. If a back button is
   acceptable, firmware macros (F) avoid the software path entirely.
3. **Is PadForge a suitable foundation?** No. Every input goes through a polled
   1 kHz pipeline. Its macro clock drifts and drops re-triggers. It would
   replace your ViGEmBus with a new driver stack. A small custom app will do
   better on every requirement you listed.
4. **Controller-native macros with accurate timing and FIFO?** Yes, in D:
   absolute-deadline scheduling on a high-resolution waitable timer with a
   final spin, plus an activation queue. Accuracy has to be proven with
   benchmark step 3.
5. **Added latency per architecture, from evidence:**
   * A/B: 0–1 ms sampling delay per input (from the code), plus pipeline time
     and HIDMaestro time, both unmeasured.
   * C: not viable.
   * D: processing plus ViGEm update, with no sampling delay. To be measured;
     ≤0.25 ms more on the busy-poll fallback.
   * E and F: ≈0.

   No end-to-end figures exist yet; §5 produces them.
6. **Best balance:** D.

## Recommendation

**Build approach D: a small native C++ passthrough + FIFO macro engine on
ViGEmBus + HidHide (both already installed). Measure it against reWASD before
switching.** Keep F (firmware macros on a back button) as an option to test,
since it is the only zero-latency path that stays within your constraints.

### Implementation plan

0. **Baseline (read-only).** Run the §4 commands and record your reWASD macro
   timeline. Run benchmark step 4 and PresentMon on reWASD and on the bare pad.
1. **Probe tool** (benchmark step 1). Decide between the Raw Input wake and the
   busy-poll designs from measured data.
2. **Passthrough core** (C++, ViGEmClient SDK). One time-critical MMCSS
   ("Games") thread:
   * on input wake: `XInputGetStateEx` → apply overlay → `vigem_target_x360_update`
   * forward rumble from the ViGEm notification to the physical pad with
     `XInputSetState`
   * neutral-state and cleanup on exit or unplug

   This step needs your consent to add the exe to HidHide's allow list and to
   hide the pad (a HidHide setting change). Measure benchmark steps 2–3.
3. **Macro engine.** Macro definitions in JSON: steps are `{t_ms, buttons
   set/clear mask, LX/LY/RX/RY/LT/RT value or "pass"}`.
   * Chord detector: L1/R1 held + Square rising edge → enqueue left/right
     macro. Square is withheld for that press. L1/R1 keep passing through
     unless a step overrides them.
   * FIFO queue with a cap, e.g. 2, so mashing can't build a long backlog.
   * Deadlines are absolute: `start + t_ms`, never `now + delay`.
   * Per-field overlay: a macro owns only the fields its current step sets.
     Everything else is live input.
4. **Validation.** Benchmark step 3 for macro timing error distributions
   against reWASD. Rocket League free-play wall-dash consistency. PresentMon
   frame times. Camera end-to-end (step 4).
5. **Hardening.** Device re-plug, ViGEm target recreation, a tray toggle, and
   a guard that releases all overrides if the thread stalls.
