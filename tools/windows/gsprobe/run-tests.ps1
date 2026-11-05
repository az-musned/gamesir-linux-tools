# run-tests.ps1 — guided, read-only measurement session.
#
# Runs inspect.ps1 and gsprobe.exe in three scenarios and saves everything
# under .\results\. It never changes reWASD, GameSir Connect, HidHide or the
# controller. Between scenarios it asks YOU to put the PC in the next state
# (e.g. apply your reWASD config); skip any scenario you don't want to run.
#
#   powershell -ExecutionPolicy Bypass -File .\run-tests.ps1

$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
$exe = Join-Path $here 'gsprobe.exe'
$res = Join-Path $here 'results'
New-Item -ItemType Directory -Force -Path $res | Out-Null
if (-not (Test-Path $exe)) { Write-Host "gsprobe.exe not found next to this script. Build it first (build.bat)."; exit 1 }

function Probe($name, [string[]]$probeArgs, $instructions) {
  Write-Host "`n>>> $name"
  Write-Host "    $instructions"
  Read-Host "    Press Enter to start (or Ctrl+C to quit)" | Out-Null
  & $exe @probeArgs 2>&1 | Tee-Object -FilePath (Join-Path $res "$name.txt")
}

function Ask($q) { (Read-Host "$q [y/N]") -match '^[yY]' }

function Enum-Pick($label) {
  & $exe enum --all 2>&1 | Tee-Object -FilePath (Join-Path $res "enum-$label.txt")
}

# ── S0: no remapper, GameSir Connect closed ──────────────────────────────
Write-Host "`n=== Scenario S0: raw controller ==="
Write-Host "Put the PC in this state yourself: reWASD config NOT applied (or reWASD closed), GameSir Connect closed."
if (Ask "Run S0?") {
  & (Join-Path $here 'inspect.ps1') -Label S0
  Enum-Pick 'S0'
  Write-Host "`nLook at the list above. Note the index of the 'vendor-defined' GameSir collection (interface mi=01), if any."
  Probe 'S0-hid-idle'      @('hid','-t','10')                       "Do NOT touch the controller for 10 s (does it send reports when idle?)."
  Probe 'S0-hid-sticks'    @('hid','-t','30','--csv',"$res\S0-hid-sticks.csv") "Rotate BOTH sticks continuously for 30 s."
  Probe 'S0-xi-1000us'     @('xinput','-t','30','--period-us','1000') "Rotate both sticks continuously for 30 s."
  Probe 'S0-xi-250us'      @('xinput','-t','30','--period-us','250')  "Rotate both sticks continuously for 30 s."
  Probe 'S0-both-presses'  @('both','-t','60','--period-us','250')    "Press A (or Square/X) about once per second for 60 s; keep the sticks still."
  Probe 'S0-both-sticks'   @('both','-t','30','--period-us','250')    "Rotate both sticks continuously while tapping A for 30 s."
  $v = Read-Host "Index of the vendor-defined GameSir collection to test passively (blank = skip)"
  if ($v -ne '') {
    Probe 'S0-vendor-passive' @('hid','--dev',$v,'-t','15') "Rotate the sticks for 15 s. (Passive read only: no heartbeat is sent.)"
  }
}

# ── S1: GameSir Connect open (no changes made in it) ─────────────────────
Write-Host "`n=== Scenario S1: GameSir Connect running ==="
Write-Host "Open GameSir Connect and let it detect the pad. Do not change any setting. reWASD config still NOT applied."
if (Ask "Run S1?") {
  Enum-Pick 'S1'
  Probe 'S1-hid-sticks' @('hid','-t','20') "Rotate both sticks for 20 s."
  $v = Read-Host "Index of the vendor-defined GameSir collection (blank = skip)"
  if ($v -ne '') { Probe 'S1-vendor-passive' @('hid','--dev',$v,'-t','20') "Rotate the sticks for 20 s." }
}

# ── S2: your working reWASD setup ────────────────────────────────────────
Write-Host "`n=== Scenario S2: reWASD config applied ==="
Write-Host "Close GameSir Connect. Apply your usual reWASD config exactly as you play with it."
if (Ask "Run S2?") {
  & (Join-Path $here 'inspect.ps1') -Label S2
  Enum-Pick 'S2'
  Write-Host "`nIn the list above: is the GameSir still listed / openable (physical hidden?), and which XInput slot is the reWASD/virtual pad?"
  $slot = Read-Host "XInput slot of the virtual (reWASD) pad (blank = skip)"
  if ($slot -ne '') {
    Probe 'S2-xi-virtual-250us' @('xinput','--slot',$slot,'-t','30','--period-us','250') "Rotate both sticks for 30 s."
    $dev = Read-Host "HID index of the PHYSICAL GameSir game/IG_ collection if still listed with 'shared-read open: ok' (blank = skip)"
    if ($dev -ne '') {
      Probe 'S2-both-physical-vs-virtual' @('both','--all','--dev',$dev,'--slot',$slot,'-t','60','--period-us','250') `
        "Press A about once per second for 60 s (measures physical-HID -> reWASD-virtual delay, +<=0.25 ms sampling)."
    }
    Probe 'S2-macro-timeline' @('xinput','--slot',$slot,'-t','20','--period-us','250','--csv',"$res\S2-macro-timeline.csv") `
      "Trigger your L1+Square macro 5 times, ~2 s apart (records reWASD's macro output timeline)."
  }
}

Write-Host "`nDone. Zip the 'results' folder and share it (it contains device IDs and file paths, no personal data beyond that)."
