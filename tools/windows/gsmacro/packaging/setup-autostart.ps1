# setup-autostart.ps1 - make gsmacro run by itself in the background at login.
#
# Run once (it asks for admin rights itself). It adds two things, both removed
# again by:  setup-autostart.ps1 -Uninstall
#   1. a per-user startup entry (HKCU\...\Run, value "gsmacro"):
#        "<this folder>\gsmacro.exe" --autostart
#      gsmacro then starts hidden with a tray icon, starts Live Mode by itself,
#      and arms the macros only while RocketLeague.exe runs.
#   2. a scheduled task "gsmacro slot0" with no trigger, run with highest
#      privileges, that gsmacro starts (no UAC prompt) when its virtual pad is
#      not in XInput slot 0: it briefly disables the GameSir's XInput interface
#      so the virtual pad can take slot 0 (player 1), then re-enables it.
#      See start-gsmacro-slot0.ps1 -Mode boot; log: %TEMP%\gsmacro-slot0.log
# No drivers, HidHide, reWASD or controller settings are changed.
param([switch]$Uninstall, [switch]$Quiet)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$exe = Join-Path $here 'gsmacro.exe'
$helper = Join-Path $here 'start-gsmacro-slot0.ps1'
$taskName = 'gsmacro slot0'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Uninstall) { $a += '-Uninstall' }
    if ($Quiet) { $a += '-Quiet' }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $a
    exit
}

try {
    if ($Uninstall) {
        Write-Host "Removing gsmacro background start..." -ForegroundColor Cyan
        if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
            Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
            Write-Host "  removed scheduled task '$taskName'"
        } else { Write-Host "  scheduled task '$taskName' was not registered" }
        if (Get-ItemProperty -Path $runKey -Name gsmacro -ErrorAction SilentlyContinue) {
            Remove-ItemProperty -Path $runKey -Name gsmacro
            Write-Host "  removed startup entry 'gsmacro'"
        } else { Write-Host "  startup entry 'gsmacro' was not present" }
        Write-Host "Done. gsmacro no longer starts by itself (a running copy keeps running until you Exit it from the tray)." -ForegroundColor Green
    } else {
        if (-not (Test-Path $exe)) { throw "gsmacro.exe not found next to this script ($here)" }
        if (-not (Test-Path $helper)) { throw "start-gsmacro-slot0.ps1 not found next to this script ($here)" }
        Write-Host "Setting up gsmacro background start..." -ForegroundColor Cyan

        $action = New-ScheduledTaskAction -Execute 'powershell.exe' `
            -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$helper`" -Mode boot"
        $principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Highest
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
            -ExecutionTimeLimit (New-TimeSpan -Minutes 3) -MultipleInstances IgnoreNew
        Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings `
            -Description 'Started by gsmacro --autostart: briefly disables the GameSir XInput interface so the virtual pad gets XInput slot 0. No trigger of its own.' `
            -Force | Out-Null
        Write-Host "  scheduled task '$taskName' registered (runs elevated only when gsmacro asks; no trigger)"

        Set-ItemProperty -Path $runKey -Name gsmacro -Value "`"$exe`" --autostart"
        Write-Host "  startup entry 'gsmacro' = `"$exe`" --autostart"

        $t = Get-ScheduledTask -TaskName $taskName
        $r = (Get-ItemProperty -Path $runKey -Name gsmacro).gsmacro
        Write-Host ""
        Write-Host "Check: task state=$($t.State), runlevel=$($t.Principal.RunLevel); startup entry=$r" -ForegroundColor Green
        Write-Host "Sign out and back in (or restart) to start it, or run: `"$exe`" --autostart"
        Write-Host "Undo: run this script with -Uninstall."
    }
    "RESULT ok $(if ($Uninstall) { 'Background start removed.' } else { 'Background start set up: gsmacro starts by itself at every login.' })" |
        Set-Content (Join-Path $env:TEMP 'gsmacro-admin.log') -Encoding utf8
} catch {
    Write-Host "FAILED: $($_.Exception.Message)" -ForegroundColor Red
    "RESULT fail Background start setup failed: $($_.Exception.Message)" | Set-Content (Join-Path $env:TEMP 'gsmacro-admin.log') -Encoding utf8
}
if (-not $Quiet) { Read-Host 'Enter to close' }
