# Hardware validation checklist (first session at the PC)

Nothing in this list has been run yet. Work top to bottom, write each result
down, and stop at the first failure. Keep Rocket League closed until
section F.

**Before you start:** unapply your reWASD config (don't change it), close
GameSir Connect, plug the pad in by cable.

## A. Offline sanity (no drivers involved)
- [ ] `gsm_tests.exe` → `72 test cases, 72 passed`.
- [ ] `gsm_bench.exe mock --out bench-mock.txt` completes. Save the file;
      these are your PC's software-floor numbers.
- [ ] `gsmacro.exe` starts in Demo Mode. Play "L1 + Square" and "R1 + Square
      x5 rapid (FIFO)" and check the timeline looks like the screenshots.

## B. Input path (read-only)
- [ ] `diagnostics\gsprobe\run-tests.ps1` scenario S0 (earlier investigation
      tool). Keep `results\`.
- [ ] `gsm_bench.exe slots` lists the GameSir as `3537:103D`.
- [ ] `gsm_bench.exe poll -t 20 --out bench-poll.txt` while moving both
      sticks: input changes/s > 0, submit failures 0.
- [ ] Setup step 1 is green.

## C. Virtual pad
- [ ] Setup step 2 is green (ViGEmClient.dll loads, bus reachable).
- [ ] Setup step 3: Live Mode starts. `joy.cpl` shows *Controller (XBOX 360
      For Windows)*. The status bar shows the virtual slot.
- [ ] `gsm_bench.exe slots` now also lists `045E:028E`.
- [ ] Stop Live Mode: the virtual pad disappears from `joy.cpl` within ~1 s.
- [ ] Start again and close gsmacro with the X button: the pad disappears
      too.

## D. Passthrough (macros OFF)
- [ ] In `joy.cpl`, test every button, both sticks (full circle), both
      triggers (0 to full), the D-pad, and Guide if shown.
- [ ] The Live self-test reads ≥ 99 % after moving everything for ~20 s.
- [ ] Square alone, L1 and R1 behave normally.
- [ ] Rumble: if a tester or game sends rumble, the GameSir vibrates.
      Untested code path, so note the result either way.
- [ ] `gsm_bench.exe live --allow-virtual-pad -t 20 --out bench-live.txt`
      (gsmacro GUI closed) while moving the sticks. Keep the file.

## E. Hiding and duplicates (manual HidHide step)
- [ ] Add `gsmacro.exe` in HidHide's Applications tab. Hide the GameSir and
      enable hiding.
- [ ] `joy.cpl` shows exactly one controller (the virtual one), and gsmacro
      still shows "GameSir connected".
- [ ] Unhide in HidHide: the physical pad is back in `joy.cpl` (undo works).

## F. Macros
- [ ] Enter your real reWASD macro steps. Save.
- [ ] Arm macros (setup step 7).
- [ ] **Test** each side: watch the virtual output pad and the timeline.
- [ ] L1 held + Square: left macro. L1 keeps working (air roll) during and
      after it.
- [ ] R1 held + Square: right macro.
- [ ] Square alone: normal action, no macro, no delay.
- [ ] Mash L1 + Square 5× fast: macros run one after another, never
      overlapping. Extra presses beyond the queue are dropped (Diagnostics
      counts them).
- [ ] Press Cancel macros mid-macro: everything releases at once.
- [ ] Unplug the pad mid-macro: output goes neutral. Replug: it reconnects
      within ~1 s and nothing fires on its own.
- [ ] `gsm_bench.exe live --allow-virtual-pad -t 30 --trigger 20 --config
      %APPDATA%\gsmacro\gsmacro.cfg --out bench-live-macros.txt`
      (GUI closed, don't touch the pad).

## G. Compare with reWASD (see BENCHMARKING.md)
- [ ] reWASD macro timeline via `gsm_bench.exe capture`.
- [ ] gsmacro macro timeline via `gsm_bench.exe capture`.
- [ ] Optional: camera end-to-end comparison in Rocket League free play.

## H. In game (free play only)
- [ ] Rocket League sees one controller, and inputs feel normal with macros
      OFF.
- [ ] Wall dashes with macros ON.
- [ ] Frame time is stable (PresentMon optional).
