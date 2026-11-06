#!/usr/bin/env bash
# Reproducible portable Windows release, cross-compiled on Linux.
#   requirements: g++-mingw-w64-x86-64-posix, cmake, ninja, git, python3, zip support in python
#   usage: packaging/build-release.sh [OUTPUT_DIR]
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"          # tools/windows/gsmacro
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="$(mkdir -p "${1:-$HERE/dist}" && cd "${1:-$HERE/dist}" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

VIGEM_TAG=v1.21.222.0
VIGEM_COMMIT=9e842ba1c3a6efbb90d9b7e9346a55b1a3d10494
CXX=x86_64-w64-mingw32-g++-posix

echo "== 1/4 ViGEmClient.dll from the official source ($VIGEM_TAG)"
git clone -q --depth 1 --branch "$VIGEM_TAG" https://github.com/nefarius/ViGEmClient "$WORK/ViGEmClient"
got="$(git -C "$WORK/ViGEmClient" rev-parse HEAD)"
[ "$got" = "$VIGEM_COMMIT" ] || { echo "ViGEmClient commit mismatch: $got"; exit 1; }
mkdir -p "$WORK/shim"   # MinGW headers are lower-case; the source includes <Windows.h>, <SetupAPI.h>
echo '#include <windows.h>' > "$WORK/shim/Windows.h"
echo '#include <setupapi.h>' > "$WORK/shim/SetupAPI.h"
$CXX -O2 -std=c++17 -shared -DVIGEM_DYNAMIC -DVIGEM_EXPORTS -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNDEBUG \
    -I "$WORK/shim" -I "$WORK/ViGEmClient/include" -I "$WORK/ViGEmClient/src" \
    "$WORK/ViGEmClient/src/ViGEmClient.cpp" -o "$WORK/ViGEmClient.dll" \
    -static -static-libgcc -static-libstdc++ -lsetupapi
x86_64-w64-mingw32-strip "$WORK/ViGEmClient.dll"

echo "== 2/4 gsmacro, gsm_bench, gsm_tests"
cmake -S "$HERE" -B "$WORK/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$HERE/packaging/mingw-w64.cmake" -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C "$WORK/build" >/dev/null
for f in gsmacro gsm_bench gsm_tests; do x86_64-w64-mingw32-strip "$WORK/build/$f.exe"; done

echo "== 3/4 gsprobe (diagnostics)"
$CXX -O2 -std=c++17 -static -s -o "$WORK/gsprobe.exe" "$REPO/tools/windows/gsprobe/gsprobe.cpp" -lsetupapi -lhid -lcfgmgr32 -ladvapi32

echo "== 4/4 assemble"
PKG="$WORK/gsmacro"
mkdir -p "$PKG/examples" "$PKG/docs" "$PKG/licenses" "$PKG/diagnostics/gsprobe" "$PKG/screenshots"
cp "$WORK/build/gsmacro.exe" "$WORK/build/gsm_bench.exe" "$WORK/build/gsm_tests.exe" "$WORK/ViGEmClient.dll" "$PKG/"
cp "$HERE"/config/examples/*.cfg "$PKG/examples/"
cp "$HERE/README.md" "$PKG/README.md"
cp "$HERE/packaging/start-gsmacro-slot0.ps1" "$HERE/packaging/setup-autostart.ps1" "$HERE/packaging/hidhide-setup.ps1" "$PKG/"
cp "$HERE"/docs/SETUP.md "$HERE"/docs/HARDWARE-CHECKLIST.md "$HERE"/docs/BENCHMARKING.md "$PKG/docs/"
cp "$HERE"/docs/screenshots/*.png "$PKG/screenshots/" 2>/dev/null || true
cp "$WORK/gsprobe.exe" "$REPO"/tools/windows/gsprobe/{inspect.ps1,run-tests.ps1,README.md} "$PKG/diagnostics/gsprobe/"
cp "$REPO/LICENSE" "$PKG/licenses/gsmacro-LICENSE.txt"
cp "$HERE/third_party/imgui/LICENSE.txt" "$PKG/licenses/dear-imgui-LICENSE.txt"
cp "$WORK/ViGEmClient/LICENSE" "$PKG/licenses/ViGEmClient-LICENSE.txt"
cp /usr/share/doc/mingw-w64-x86-64-dev/copyright "$PKG/licenses/mingw-w64-runtime-copyright.txt"
cp /usr/share/doc/gcc-mingw-w64-base/copyright "$PKG/licenses/gcc-runtime-library-exception.txt"
cp "$HERE/packaging/THIRD_PARTY_NOTICES.txt" "$PKG/"
{
  echo "gsmacro portable Windows x64 build"
  echo "built:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "source:       $(git -C "$REPO" rev-parse HEAD)$(git -C "$REPO" diff --quiet || echo ' (+local changes)')"
  echo "compiler:     $($CXX --version | head -1)"
  echo "ViGEmClient:  $VIGEM_TAG ($VIGEM_COMMIT), built from source as ViGEmClient.dll"
  echo "Dear ImGui:   $(cat "$HERE/third_party/imgui/VERSION.txt")"
  echo "status:       built and tested offline; NOT yet validated with the real controller / ViGEmBus / HidHide"
} > "$PKG/BUILD-INFO.txt"
( cd "$PKG" && find . -type f ! -name SHA256SUMS.txt -print0 | sort -z | xargs -0 sha256sum ) > "$WORK/SHA256SUMS.txt"
mv "$WORK/SHA256SUMS.txt" "$PKG/"
python3 -I - "$WORK" "$OUT/gsmacro-windows-x64.zip" <<'PY'
import os, sys, zipfile
root, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(os.path.join(root, 'gsmacro')):
        for f in sorted(files):
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, root))
PY
rm -rf "$OUT/gsmacro"; cp -r "$PKG" "$OUT/gsmacro"
( cd "$OUT" && sha256sum gsmacro-windows-x64.zip > gsmacro-windows-x64.zip.sha256 )
echo "done: $OUT/gsmacro-windows-x64.zip"
