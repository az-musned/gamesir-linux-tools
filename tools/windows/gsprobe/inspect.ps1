# inspect.ps1 — read-only snapshot of the controller / driver / reWASD state.
#
# Changes nothing: it only lists processes, services, PnP devices and their
# properties, reads registry values and lists/greps reWASD config files.
# Run from an elevated PowerShell for the most complete driver listing
# (pnputil); everything else works unelevated.
#
#   powershell -ExecutionPolicy Bypass -File .\inspect.ps1 -Label S0

param([string]$Label = "snapshot")

$out = Join-Path $PSScriptRoot "results\inspect-$Label.txt"
New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
Start-Transcript -Path $out -Force | Out-Null

function Section($t) { Write-Host "`n===== $t =====" }

Section "System"
Get-CimInstance Win32_OperatingSystem | Format-List Caption, Version, BuildNumber
Write-Host ("Elevated: " + ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))

Section "Relevant processes"
Get-Process | Where-Object { $_.ProcessName -match 'rewasd|gamesir|hidhide|steam|rocketleague|ds4windows|padforge|x360ce' } |
  Format-Table ProcessName, Id, Path -AutoSize

Section "Relevant services / drivers (Get-Service)"
Get-Service | Where-Object { $_.Name -match 'rewasd|hidhide|vigem|gamesir|xusb|xboxgip|xinputhid|hidusb' -or $_.DisplayName -match 'rewasd|hidhide|vigem|gamesir' } |
  Format-Table Name, DisplayName, Status, StartType -AutoSize

Section "PnP devices of interest (present)"
$devs = Get-PnpDevice -PresentOnly | Where-Object {
  $_.InstanceId -match 'VID_3537' -or $_.FriendlyName -match 'reWASD|ViGEm|Virtual|Xbox|XINPUT|game controller|HidHide' -or $_.Class -match 'XnaComposite|XboxComposite'
}
$devs | Sort-Object InstanceId | Format-Table Status, Class, FriendlyName, InstanceId -AutoSize -Wrap

$keys = 'DEVPKEY_Device_Service','DEVPKEY_Device_UpperFilters','DEVPKEY_Device_LowerFilters','DEVPKEY_Device_Parent',
        'DEVPKEY_Device_DriverProvider','DEVPKEY_Device_DriverVersion','DEVPKEY_Device_DriverInfPath','DEVPKEY_Device_Children'
foreach ($d in ($devs | Sort-Object InstanceId)) {
  Write-Host "`n--- $($d.InstanceId)  [$($d.FriendlyName)]"
  foreach ($k in $keys) {
    $p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName $k -ErrorAction SilentlyContinue
    if ($p -and $p.Data) { Write-Host ("  {0,-30} {1}" -f $k.Replace('DEVPKEY_Device_',''), (($p.Data | ForEach-Object { "$_" }) -join ', ')) }
  }
}

Section "Class filters"
$classes = @{
  'HIDClass'      = '{745a17a0-74d3-11d0-b6fe-00a0c90f57da}'
  'XnaComposite'  = '{d61ca365-5af4-4486-998b-9db4734c6ca3}'
  'XboxComposite' = '{05f5cfe2-4733-4950-a6bb-07aad01a3a84}'
  'USB'           = '{36fc9e60-c465-11cf-8056-444553540000}'
}
foreach ($c in $classes.GetEnumerator()) {
  $k = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$($c.Value)"
  $p = Get-ItemProperty -Path $k -ErrorAction SilentlyContinue
  Write-Host ("{0,-14} upper={1}  lower={2}" -f $c.Key, (($p.UpperFilters) -join ','), (($p.LowerFilters) -join ','))
}

Section "HidHide configuration (registry, read only)"
$hh = 'HKLM:\SYSTEM\CurrentControlSet\Services\HidHide\Parameters'
if (Test-Path $hh) {
  (Get-Item $hh).Property | ForEach-Object {
    $v = (Get-ItemProperty -Path $hh -Name $_).$_
    Write-Host ("  {0} = {1}" -f $_, (($v | ForEach-Object { "$_" }) -join ' | '))
  }
} else { Write-Host "  (no $hh)" }

Section "Third-party input drivers in the driver store (pnputil)"
try { pnputil /enum-drivers | Select-String -Pattern 'rewasd|vigem|hidhide|gamesir|nefarius|disc soft' -Context 3,5 | ForEach-Object { $_.ToString() } }
catch { Write-Host "  pnputil failed: $_" }

Section "reWASD configuration files (listing + keyword lines, read only)"
$dirs = @("$env:PUBLIC\Documents\reWASD", "$env:ProgramData\reWASD", "$env:USERPROFILE\Documents\reWASD", "$env:LOCALAPPDATA\reWASD", "$env:APPDATA\reWASD")
foreach ($dir in $dirs) {
  if (-not (Test-Path $dir)) { continue }
  Write-Host "`n[$dir]"
  Get-ChildItem $dir -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -match '\.(rewasd|json|xml|ini|cfg)$' } |
    ForEach-Object {
      Write-Host ("  {0}  {1:N0} bytes  {2}" -f $_.FullName, $_.Length, $_.LastWriteTime)
      if ($_.Length -lt 5MB) {
        Select-String -Path $_.FullName -Pattern 'virtual|emulat|controllerType|gamepadType|hide|native|macro|delay|stick' -AllMatches -ErrorAction SilentlyContinue |
          Select-Object -First 60 | ForEach-Object { Write-Host ("      L{0}: {1}" -f $_.LineNumber, $_.Line.Trim().Substring(0, [Math]::Min(200, $_.Line.Trim().Length))) }
      }
    }
}

Stop-Transcript | Out-Null
Write-Host "Saved: $out"
