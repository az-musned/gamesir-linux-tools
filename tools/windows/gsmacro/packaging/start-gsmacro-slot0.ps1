# start-gsmacro-slot0.ps1 - start gsmacro so its virtual pad gets XInput slot 0
# (games treat slot 0 as player 1) without unplugging the controller.
#
# Windows hands out XInput slots in connection order and a HidHide-hidden pad
# still holds its slot. This does in software what unplugging does:
#   1. an elevated helper disables the GameSir's XInput interface (&MI_00),
#      which is what holds the XInput slot (fallback: the whole USB device,
#      which Windows refused on the target PC as a "critical system device")
#   2. gsmacro is opened normally (as you, not as admin)
#   3. you click Start Live Mode; the virtual pad takes slot 0
#   4. the helper re-enables the GameSir (it lands in slot 1, hidden)
# Step 4 ALWAYS runs: on success, on timeout, or if this window is closed
# (the helper watches it). A later run also re-enables a pad that an
# interrupted run left disabled. Manual recovery from an admin prompt:
#   pnputil /enable-device "<instance id printed below>"
# or Device Manager > the GameSir device > Enable.
#
# Device state only: no drivers, no HidHide / reWASD / controller settings.
# One UAC prompt (for the helper). Run it from the shortcut, not elevated.
param(
    # launch: interactive launcher; cycle/enable: its elevated helper;
    # boot: run by the "gsmacro slot0" scheduled task when gsmacro --autostart asks
    [ValidateSet('launch', 'cycle', 'enable', 'boot')] [string]$Mode = 'launch',
    [string]$Id = '',
    [int]$ParentPid = 0,
    [int]$TimeoutSec = 180
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$exe = Join-Path $here 'gsmacro.exe'
$bench = Join-Path $here 'gsm_bench.exe'

function Slot0 {
    $line = & $bench slots 2>$null | Select-String 'slot 0:'
    # "  slot 0: 045E:028E  <- Xbox 360 identity ..." -> "045E:028E"; "  slot 0: -" -> "-"
    if ($line) { (($line.Line -replace '.*slot 0:\s*', '').Trim() -split '\s+')[0] } else { '?' }
}
function GameSirParent {
    # The composite USB device (no MI_/IG_ part): disabling it removes every interface, like the cable.
    Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match '^USB\\VID_3537&PID_[0-9A-F]{4}\\[^&]+$' -and $_.Problem -ne 'CM_PROB_PHANTOM' } |
        Select-Object -First 1
}
function PendingDisable($devId) {   # CONFIGFLAG_DISABLED stored (applies at boot even if the device runs now)
    $f = (Get-PnpDeviceProperty -InstanceId $devId -KeyName DEVPKEY_Device_ConfigFlags -ErrorAction SilentlyContinue).Data
    [bool]($f -band 1)
}
function IsDisabled($d) { $d -and $d.Status -ne 'OK' -and ($d.Problem -eq 'CM_PROB_DISABLED' -or $d.ConfigManagerErrorCode -eq 22) }
function Enable-Pad($devId) {
    & pnputil /enable-device "$devId" | Out-Null
    for ($i = 0; $i -lt 20; $i++) {
        $d = Get-PnpDevice -InstanceId $devId -ErrorAction SilentlyContinue
        if ($d -and $d.Status -eq 'OK') { return $true }
        Start-Sleep -Milliseconds 250
    }
    $false
}
$logFile = Join-Path $env:TEMP 'gsmacro-slot0.log'
function Log([string]$m) { Add-Content -Path $logFile -Value "$(Get-Date -Format 'HH:mm:ss.fff') $m" -Encoding utf8 }
function LastState { if (Test-Path $logFile) { (Get-Content $logFile | Select-String 'STATE ' | Select-Object -Last 1).Line } }
function Start-Helper([string]$helperMode, [string]$devId, [switch]$Wait) {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', "`"$PSCommandPath`"",
           '-Mode', $helperMode, '-Id', "`"$devId`"", '-ParentPid', $PID, '-TimeoutSec', $TimeoutSec)
    try { Start-Process powershell.exe -Verb RunAs -ArgumentList $a -Wait:$Wait; $true } catch { $false }  # false = UAC declined
}

# ── elevated helper ────────────────────────────────────────────────────────
if ($Mode -eq 'enable') { [void](Enable-Pad $Id); exit }
if ($Mode -eq 'boot') {
    # Scheduled task: gsmacro --autostart asks for this BEFORE it starts Live Mode
    # (it waits until the XInput interface is off, then creates its pad in slot 0).
    # No launcher window to watch.
    $pad = GameSirParent
    if (-not $pad) { Log 'boot: GameSir not found'; exit }
    $Id = $pad.InstanceId
    $ParentPid = 0
    if ($TimeoutSec -gt 60) { $TimeoutSec = 60 }
    Log "boot: requested by gsmacro --autostart"
    $Mode = 'cycle'
}
if ($Mode -eq 'cycle') {
    $disabled = @()
    try {
        Log "helper start: admin=$(([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) id=[$Id] parent=$ParentPid"
        # Disable the XInput interface (MI_00), which is what holds the XInput slot; if that
        # fails, try the whole USB device (on the target PC Windows refuses the latter as a
        # "critical system device", which costs ~10 s, so it is only the fallback).
        $xusbDev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\VID_3537&PID_*&MI_00\*' -and $_.Problem -ne 'CM_PROB_PHANTOM' } | Select-Object -First 1
        if ($xusbDev -and (IsDisabled $xusbDev)) {
            # Already off (e.g. a disable stored by an earlier run took effect at boot): just wait and re-enable.
            Log "XInput interface already disabled [$($xusbDev.InstanceId)] - using it as is"
            $disabled += $xusbDev.InstanceId
        } else {
            foreach ($target in @($xusbDev.InstanceId, $Id) | Where-Object { $_ }) {
                $out = (& pnputil /disable-device "$target" 2>&1 | Out-String).Trim()
                $code = $LASTEXITCODE
                Log "pnputil /disable-device [$target] exit=$code :: $($out -replace '\s+', ' ')"
                Start-Sleep -Milliseconds 700
                $st = Get-PnpDevice -InstanceId $target -ErrorAction SilentlyContinue
                Log "  -> status=$($st.Status) problem=$($st.Problem) pendingDisable=$(PendingDisable $target)"
                if ($st -and $st.Status -ne 'OK') { $disabled += $target; break }
                # Refused while running (e.g. exit 3010 "reboot needed": some process holds it open).
                # Windows may have STORED the disable for the next boot - undo that now.
                if ($code -eq 3010 -or (PendingDisable $target)) {
                    $ok = Enable-Pad $target
                    Log "  undo stored disable [$target] ok=$ok pendingDisable=$(PendingDisable $target)"
                }
            }
        }
        if (-not $disabled) { Log 'STATE failed: no device could be disabled'; exit }
        Log "STATE disabled $($disabled -join ',')"
        $t0 = Get-Date
        while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
            if ((Slot0) -eq '045E:028E') { Log 'slot 0 is the virtual pad'; break }
            if ($ParentPid -and -not (Get-Process -Id $ParentPid -ErrorAction SilentlyContinue)) { Log 'launcher closed'; break }
            Start-Sleep -Milliseconds 300
        }
    } catch {
        Log "ERROR $($_.Exception.Message)"
    } finally {
        foreach ($t in $disabled) { $ok = Enable-Pad $t; Log "re-enable [$t] ok=$ok" }
        Log 'STATE done'
    }
    exit
}

# ── launcher (runs as you) ─────────────────────────────────────────────────
Write-Host "gsmacro slot-0 launcher" -ForegroundColor Cyan
$gsmRunning = [bool](Get-Process gsmacro -ErrorAction SilentlyContinue)
$pad = GameSirParent

if (IsDisabled $pad) {
    Write-Host "The GameSir was left disabled by an interrupted run - re-enabling it (admin prompt)..." -ForegroundColor Yellow
    if (-not (Start-Helper 'enable' $pad.InstanceId -Wait)) { Write-Host "Admin prompt declined. Enable it in Device Manager." -ForegroundColor Red; Read-Host 'Enter to close'; exit }
    $pad = GameSirParent
}
if (-not $pad) {
    Write-Host "GameSir not connected. Opening gsmacro: click Start Live Mode, THEN plug the pad in."
    if (-not $gsmRunning) { Start-Process $exe -WorkingDirectory $here }
    Read-Host 'Enter to close'; exit
}
if ((Slot0) -eq '045E:028E') {
    Write-Host "gsmacro's virtual pad is already in slot 0 - nothing to do." -ForegroundColor Green
    if (-not $gsmRunning) { Start-Process $exe -WorkingDirectory $here }
    Read-Host 'Enter to close'; exit
}

$id = $pad.InstanceId
Write-Host "GameSir: $id"
Write-Host "1/4 Asking for admin rights to disable the GameSir (software unplug)..."
Remove-Item $logFile -ErrorAction SilentlyContinue
Log "launcher: id=[$id] pid=$PID"
if (-not (Start-Helper 'cycle' $id)) { Write-Host "Admin prompt declined - nothing was changed." -ForegroundColor Yellow; Read-Host 'Enter to close'; exit }
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 30 -and -not ((LastState) -match 'STATE (disabled|failed|done)')) { Start-Sleep -Milliseconds 200 }
$st = LastState
if ($st -notmatch 'STATE disabled') {
    Write-Host "The GameSir did not get disabled (helper said: $(if ($st) { $st } else { 'nothing - it may not have started' }))." -ForegroundColor Red
    Write-Host "Log: $logFile"
    if (Test-Path $logFile) { Get-Content $logFile | ForEach-Object { "    $_" } }
    Read-Host 'Enter to close'; exit
}
Write-Host "    disabled ($($st -replace '.*STATE disabled ', ''))" -ForegroundColor Green

if ($gsmRunning) {
    Write-Host "2/4 gsmacro is already open: click Stop Live Mode, then Start Live Mode." -ForegroundColor Yellow
} else {
    Write-Host "2/4 Opening gsmacro..."
    Start-Process $exe -WorkingDirectory $here
}
Write-Host "3/4 In gsmacro, click Start Live Mode (it will say the GameSir is not found - expected)." -ForegroundColor Yellow
Write-Host "    Waiting for its virtual pad to take slot 0 (up to $TimeoutSec s; closing this window re-enables the pad)..."
while ((Get-Date) - $t0 -lt [TimeSpan]::FromSeconds($TimeoutSec + 15) -and (LastState) -notmatch 'STATE done') { Start-Sleep -Milliseconds 300 }
$reen = Get-Content $logFile -ErrorAction SilentlyContinue | Select-String 're-enable'
Write-Host "4/4 GameSir re-enabled: $(if ($reen -and -not ($reen -match 'ok=False')) { 'yes' } else { 'NOT CONFIRMED - see ' + $logFile + ' and the recovery note in this script' })"
Start-Sleep 2
Write-Host ""
Write-Host "What games see now (HidHide-hidden pads are not listed):"
& $bench slots | Select-String 'slot '
if ((Slot0) -eq '045E:028E') { Write-Host "OK: gsmacro's pad is player 1. Arm macros when ready." -ForegroundColor Green }
else { Write-Host "Slot 0 is not gsmacro's pad (timed out?). Click Start Live Mode and run this again." -ForegroundColor Yellow }
Read-Host 'Enter to close'
