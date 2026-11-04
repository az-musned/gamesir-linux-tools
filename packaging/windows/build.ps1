<#
  Build the standalone Windows version of Deadband.

    powershell -ExecutionPolicy Bypass -File packaging\windows\build.ps1

  Creates .venv-build\ (a private virtual environment), installs the runtime
  and build dependencies into it, runs the test suite, and builds
  dist\Deadband\ with PyInstaller. Nothing is installed system-wide and no
  driver is touched.

  Options:
    -SkipTests   build without running the tests first
    -Zip         also write dist\Deadband-windows-x64.zip
#>
param([switch]$SkipTests, [switch]$Zip)
$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root

# A real Python 3.10+ (the Microsoft Store "python" alias is a stub that opens the Store).
if (Get-Command py -ErrorAction SilentlyContinue) { $pyExe = 'py'; $pyArgs = @('-3') }
else { $pyExe = 'python'; $pyArgs = @() }
& $pyExe @pyArgs -c "import sys; assert sys.version_info >= (3, 10), sys.version" 2>$null
if ($LASTEXITCODE -ne 0) { throw 'Python 3.10 or newer is required (python.org installer, "Add to PATH").' }

$venv = Join-Path $root '.venv-build'
if (-not (Test-Path (Join-Path $venv 'Scripts\python.exe'))) {
    & $pyExe @pyArgs -m venv $venv
}
$vpy = Join-Path $venv 'Scripts\python.exe'

& $vpy -m pip install --upgrade pip
& $vpy -m pip install -r packaging\windows\requirements.txt pyinstaller pillow pytest
if ($LASTEXITCODE -ne 0) { throw 'pip install failed' }

if (-not $SkipTests) {
    $env:QT_QPA_PLATFORM = 'offscreen'
    & $vpy -m pytest -q tests
    if ($LASTEXITCODE -ne 0) { throw 'tests failed (use -SkipTests to build anyway)' }
    Remove-Item Env:\QT_QPA_PLATFORM
}

& $vpy -m PyInstaller --noconfirm --clean --distpath dist --workpath build\pyinstaller packaging\windows\deadband.spec
if ($LASTEXITCODE -ne 0) { throw 'PyInstaller failed' }

# A quick self-check of the built app: the diagnostics run without Qt or a device.
& dist\Deadband\deadband-console.exe --version
& dist\Deadband\deadband-console.exe --doctor

if ($Zip) {
    $zipPath = Join-Path $root 'dist\Deadband-windows-x64.zip'
    if (Test-Path $zipPath) { Remove-Item $zipPath }
    Compress-Archive -Path dist\Deadband -DestinationPath $zipPath
    Write-Host "Wrote $zipPath"
}
Write-Host ''
Write-Host 'Built dist\Deadband\Deadband.exe'
