# hidhide-setup.ps1 - hide (or unhide) the physical controller from games
# with HidHide, so a game sees only gsmacro's virtual pad. gsmacro runs this
# (elevated, one UAC prompt) only when you click "Hide controller from games"
# or "Unhide" in the app. You can also run it yourself:
#     hidhide-setup.ps1 -Mode hide      /      hidhide-setup.ps1 -Mode unhide
#
# hide:   1. lets gsmacro.exe (next to this script) see hidden devices
#         2. hides every DualSense (USB) and GameSir game-controller entry
#            Windows knows (also ones not plugged in right now, e.g. other ports),
#            including the GameSir's XInput interface
#         3. turns HidHide's device hiding on
# unhide: removes exactly those devices from HidHide's hidden list again.
#         gsmacro stays allowed and hiding stays on (other devices you hid
#         yourself are not touched).
# Nothing else is changed: no drivers, no device settings. Result line:
#   %TEMP%\gsmacro-admin.log
param([ValidateSet('hide', 'unhide')][string]$Mode = 'hide', [switch]$Quiet)
$log = Join-Path $env:TEMP 'gsmacro-admin.log'
"$(Get-Date -Format s) hidhide-setup -Mode $Mode" | Set-Content $log -Encoding utf8
function Finish([bool]$ok, [string]$msg) {
    "RESULT $(if ($ok) { 'ok' } else { 'fail' }) $msg" | Add-Content $log -Encoding utf8
    Write-Host $msg -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
    if (-not $Quiet) { Read-Host 'Enter to close' | Out-Null }
    exit $(if ($ok) { 0 } else { 1 })
}
try {
    $cli = Join-Path $env:ProgramFiles 'Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe'
    if (-not (Test-Path $cli)) {
        $cli = Get-ChildItem $env:ProgramFiles -Directory -Filter 'Nefarius*' -ErrorAction SilentlyContinue |
            ForEach-Object { Get-ChildItem $_.FullName -Recurse -Filter HidHideCLI.exe -ErrorAction SilentlyContinue } |
            Where-Object { $_.FullName -match '\\x64\\' -or [Environment]::Is64BitOperatingSystem -eq $false } |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $cli) { Finish $false 'HidHide is not installed (HidHideCLI.exe not found). Install HidHide first.' }
    if (Get-Process HidHideClient -ErrorAction SilentlyContinue) {
        Finish $false 'Close the HidHide Configuration Client window first, then try again.'
    }
    $exe = Join-Path $PSScriptRoot 'gsmacro.exe'
    $devs = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
        $_.InstanceId -match '^HID\\VID_054C&PID_(0CE6|0DF2)&MI_03\\' -or   # DualSense / Edge, USB
        $_.InstanceId -match '^HID\\VID_3537&PID_103D&IG_' -or                # GameSir game-controller HID
        $_.InstanceId -match '^USB\\VID_3537&PID_103D&MI_00\\'                # GameSir XInput interface
    })
    $ids = @($devs | Select-Object -ExpandProperty InstanceId)
    $present = @($devs | Where-Object { $_.Status -eq 'OK' } | Select-Object -ExpandProperty InstanceId)
    if (-not $ids) { Finish $false 'No DualSense or GameSir was found. Plug the controller in by USB and try again.' }
    $a = @()
    if ($Mode -eq 'hide') {
        $a += '--app-reg', "`"$exe`""
        foreach ($i in $ids) { $a += '--dev-hide', "`"$i`"" }
        $a += '--cloak-on'
    } else {
        foreach ($i in $ids) { $a += '--dev-unhide', "`"$i`"" }
    }
    foreach ($d in $devs) { "  device $($d.Status) $($d.InstanceId)" | Add-Content $log -Encoding utf8 }
    $p = Start-Process -FilePath $cli -ArgumentList $a -Wait -PassThru -WindowStyle Hidden
    "  HidHideCLI exit=$($p.ExitCode)" | Add-Content $log -Encoding utf8
    if ($p.ExitCode -ne 0) { Finish $false "HidHide refused the change (exit code $($p.ExitCode))." }

    # Check what HidHide actually has now (it may skip entries for pads that aren't plugged in).
    $listed = @(& $cli --dev-list | ForEach-Object { if ($_ -match '"([^"]+)"') { $Matches[1] } })
    $listed | ForEach-Object { "  hidden-list $_" | Add-Content $log -Encoding utf8 }
    $hidden = @($ids | Where-Object { $listed -contains $_ })
    function Names($list) {
        $n = @()
        if ($list | Where-Object { $_ -match 'VID_054C' }) { $n += 'DualSense' }
        if ($list | Where-Object { $_ -match 'VID_3537&PID_103D&IG_' }) { $n += 'GameSir' }
        if ($list | Where-Object { $_ -match 'VID_3537&PID_103D&MI_00' }) { $n += 'GameSir XInput' }
        if ($n) { $n -join ', ' } else { 'nothing' }
    }
    $hasGameSir = [bool]($present | Where-Object { $_ -match 'VID_3537' })
    $note = if ($hasGameSir) { '' } else { ' GameSir not plugged in: plug it in and click Hide again to hide it too.' }
    if ($Mode -eq 'hide') {
        $missing = @($present | Where-Object { $listed -notcontains $_ })
        if ($missing) { Finish $false "HidHide did not take: $(Names $missing). Close HidHide's window and try again." }
        if (-not $hidden) { Finish $false "Nothing was hidden (plug the controller in by USB first).$note" }
        # The installer leaves this flag so gsmacro finishes hiding later if needed; done now.
        Remove-Item (Join-Path $env:APPDATA 'gsmacro\hide-pending') -Force -ErrorAction SilentlyContinue
        Finish $true "Done. Hidden from games: $(Names $hidden). Replug the controller once.$note"
    } else {
        if ($hidden) { Finish $false "Still hidden: $(Names $hidden). Close HidHide's window and try again." }
        Finish $true 'Done. Controllers are visible to games again. Replug the controller once.'
    }
} catch {
    Finish $false "Failed: $($_.Exception.Message)"
}
