# Benchmarking procedure (gsmacro vs reWASD)

## What software can and cannot measure

| Measured by gsm_bench (software timing) | Not measurable in software |
|---|---|
| How often the controller's XInput state changes, and the poll timer error | Time inside the controller (scan, debounce, ADC) |
| gsmacro processing time per update ("engine cycle") | USB transfer and the driver stack before XInput |
| `vigem_target_x360_update` cost ("output submit") | When Rocket League samples input (120 Hz physics tick) |
| Submit → visible to any XInput reader (readback) | Render, display and scan-out |
| Macro step timing as seen on the virtual pad | **Input-to-game latency** |
| Queue behaviour, drops, scheduling jitter, CPU | |

**Never quote a gsm_bench number as input-to-game latency.** For that you
need the camera test in step 5.

## Procedure

Run everything with the PC idle, on the same USB port, with Rocket League,
GameSir Connect and other overlays closed. Save every `--out` file.

**1. Software floor (any time, no drivers):**
```
gsm_bench.exe mock --out 1-mock.txt
```

**2. Input polling and processing (read-only, no virtual pad).** Unapply
reWASD first and move both sticks continuously:
```
gsm_bench.exe poll -t 30 --period-us 500 --out 2-poll-500.txt
gsm_bench.exe poll -t 30 --period-us 250 --out 2-poll-250.txt
```
Compare input changes/s (what the pad delivers), the wake/poll error, the
engine cycle cost and the CPU cost of each poll period.

**3. gsmacro output path.** Creates one virtual pad. Close the GUI:
```
gsm_bench.exe live --allow-virtual-pad -t 30 --out 3-live-passthrough.txt
gsm_bench.exe live --allow-virtual-pad -t 30 --trigger 20 --config %APPDATA%\gsmacro\gsmacro.cfg --out 3-live-macros.txt
```
The first run reports *submit → XInput readback*. The second reports each
macro edge's timing, read back from the virtual pad. Don't touch the pad
during the second run.

**4. reWASD vs gsmacro macro timing (same capture tool for both).**
1. Apply your reWASD config. Run `gsm_bench.exe slots` to find reWASD's
   virtual pad slot, then:
   ```
   gsm_bench.exe capture --slot N -t 60 --csv 4-rewasd.csv --out 4-rewasd.txt
   ```
   Trigger L1 + Square about 15 times, ~1 s apart, then R1 + Square about
   15 times.
2. Unapply reWASD. Start gsmacro Live Mode with macros armed. Find its
   virtual slot (status bar), then:
   ```
   gsm_bench.exe capture --slot M -t 60 --csv 4-gsmacro.csv --out 4-gsmacro.txt
   ```
   Repeat the same presses.
3. Compare the *Sequence A/B* tables. Edge times are relative to each run's
   first macro edge. The standard deviation column is the run-to-run jitter.
   L1, R1 and Square are excluded by default (`--ignore`), so only
   macro-generated edges count. Capture adds up to `--period-us` (default
   250 µs) of uncertainty to each edge.

**5. End-to-end latency (the only real input-to-game number).**
* Film the controller and the screen together at ≥ 240 fps (960 fps is
  better) in Rocket League free play.
* Use one action with a clear on-screen result (jump).
* Do ≥ 50 trials each for: bare controller (no remapper, pad unhidden),
  reWASD, and gsmacro (macros armed, the action not part of a macro).
* Count frames from the button visibly bottoming out to the first changed
  frame on screen.
* Report the distributions (median, spread), not single values. Also record
  frame-time stability with PresentMon in each configuration.

## Reading the results

* **Engine cycle** and **output submit** are what gsmacro adds per update.
  On the cloud VM used for development (mock backend) they were a few µs.
  Your PC's numbers come from steps 2 and 3.
* **Poll period** adds sampling delay: on average half the period
  (250 µs at 500 µs polling), at most the full period.
* **Macro step lateness** shows how precisely each step is applied. A step
  that lands within ~1 ms of its schedule rarely changes which 120 Hz game
  tick it falls into.
