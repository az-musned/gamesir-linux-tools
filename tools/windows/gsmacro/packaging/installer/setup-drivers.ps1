# setup-drivers.ps1 - run by gsmacro-setup.exe (elevated) on the user's PC.
# Installs the two free drivers gsmacro needs, only if they are missing:
#   ViGEmBus (virtual Xbox pad)  and  HidHide (hides the real pad from games).
# Each installer is downloaded from the official GitHub release (pinned
# version), its Authenticode signature must be valid and from Nefarius,
# then it is run silently (falling back to its normal window if the silent
# run doesn't install it). Already-installed drivers are left alone.
#
# Exit code: 0 ok, 1 failed. Writes "REBOOT" to -Marker if a restart is
# needed. Log: -Log.
param([Parameter(Mandatory)][string]$Log, [Parameter(Mandatory)][string]$Marker, [switch]$DryRun)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$drivers = @(
    @{ Name = 'ViGEmBus'; Service = 'ViGEmBus'; Reboot = $false
       Url = 'https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe' },
    @{ Name = 'HidHide'; Service = 'HidHide'; Reboot = $true
       Url = 'https://github.com/nefarius/HidHide/releases/download/v1.5.230.0/HidHide_1.5.230_x64.exe' }
)

function Say([string]$m) { "$(Get-Date -Format s) $m" | Add-Content $Log -Encoding utf8 }
function Installed($d) { [bool](Get-Service $d.Service -ErrorAction SilentlyContinue) }

$reboot = $false
try {
    $dir = Join-Path $env:TEMP 'gsmacro-setup'
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($d in $drivers) {
        if (Installed $d) { Say "$($d.Name): already installed, left alone"; continue }
        if ($DryRun) { Say "$($d.Name): DRY RUN - would download $($d.Url) and install it"; continue }
        $file = Join-Path $dir ([IO.Path]::GetFileName($d.Url))
        Say "$($d.Name): downloading $($d.Url)"
        Invoke-WebRequest -Uri $d.Url -OutFile $file -UseBasicParsing
        $sig = Get-AuthenticodeSignature $file
        Say "$($d.Name): signature $($sig.Status) $($sig.SignerCertificate.Subject)"
        if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Nefarius') {
            Remove-Item $file -Force
            throw "$($d.Name) download is not signed by Nefarius - not installed."
        }
        $p = Start-Process $file -ArgumentList '/exenoui', '/qn', '/norestart' -Wait -PassThru
        Say "$($d.Name): silent install exit=$($p.ExitCode)"
        if (-not (Installed $d)) {
            Say "$($d.Name): not installed by the silent run, opening its own setup window"
            $p = Start-Process $file -Wait -PassThru
            Say "$($d.Name): setup window exit=$($p.ExitCode)"
        }
        if (-not (Installed $d)) { throw "$($d.Name) was not installed." }
        if ($d.Reboot -or $p.ExitCode -eq 3010) { $reboot = $true }
        Remove-Item $file -Force -ErrorAction SilentlyContinue
        Say "$($d.Name): installed"
    }
    if ($reboot) { 'REBOOT' | Set-Content $Marker -Encoding ascii; Say 'restart needed' }
    exit 0
} catch {
    Say "FAILED: $($_.Exception.Message)"
    $_.Exception.Message | Set-Content "$Marker.error" -Encoding utf8
    exit 1
}
