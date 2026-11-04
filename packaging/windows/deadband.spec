# PyInstaller spec for the Windows build of Deadband.
#
#   pyinstaller --noconfirm packaging\windows\deadband.spec
#
# (packaging\windows\build.ps1 does this inside a fresh virtual environment.)
# Produces dist\Deadband\ -- a self-contained folder, no Python needed:
#   Deadband.exe          the app (windowed; logs to %LOCALAPPDATA%\deadband\deadband.log)
#   deadband-console.exe  the same app with a console, for `--doctor`, `--probe`
#                         and for watching its output while testing
# A one-folder build rather than one-file: it starts faster (nothing is unpacked
# to %TEMP% on every launch) and antivirus tools trust it more.
import os
import sys

ROOT = os.path.abspath(os.path.join(SPECPATH, '..', '..'))
sys.path.insert(0, ROOT)

ICON_PNG = os.path.join(ROOT, 'assets', 'icon-1024.png')
ICON = os.path.join(SPECPATH, 'build', 'deadband.ico')
os.makedirs(os.path.dirname(ICON), exist_ok=True)
try:                                   # PNG -> multi-size .ico (needs Pillow)
    from PIL import Image
    Image.open(ICON_PNG).save(ICON, sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
                                           (64, 64), (128, 128), (256, 256)])
except Exception as e:                 # no Pillow: build without an exe icon
    print(f'[deadband.spec] no .ico ({e}); the exe keeps the default icon')
    ICON = None

# Modules imported lazily (inside functions) that static analysis can miss.
HIDDEN = [
    'doctor', 'rawhid', 'winhid', 'hostos', 'write_gate', 'register_transaction',
    'kf_cache', 'backup', 'kwin', 'mousegrab', 'version',
    'vendors.gamesir.models.cyclone2.factory', 'vendors.gamesir.models.cyclone2.led',
    'vendors.gamesir.models.cyclone2.led_factory', 'vendors.gamesir.models.g7_8k.led',
    'vendors.logitech.hidpp', 'vendors.logitech.onboard', 'vendors.logitech.remap',
    'vendors.logitech.macro', 'vendors.logitech.macros', 'vendors.logitech.config',
    'hid', 'PySide6.QtQuickControls2', 'PySide6.QtMultimedia',
]

# Qt modules the app never uses; leaving them out keeps the folder far smaller.
EXCLUDES = [
    'tkinter', 'PySide6.QtWebEngineCore', 'PySide6.QtWebEngineWidgets',
    'PySide6.QtWebEngineQuick', 'PySide6.QtWebView', 'PySide6.Qt3DCore',
    'PySide6.Qt3DRender', 'PySide6.QtDataVisualization', 'PySide6.QtCharts',
    'PySide6.QtPdf', 'PySide6.QtPdfWidgets', 'PySide6.QtBluetooth',
    'PySide6.QtDesigner', 'PySide6.QtSql', 'PySide6.QtTest',
    'PySide6.QtLocation', 'PySide6.QtPositioning', 'PySide6.QtSensors',
    'PySide6.QtRemoteObjects', 'PySide6.QtScxml', 'PySide6.QtSerialPort',
    'PySide6.QtTextToSpeech', 'PySide6.QtQuick3D',
]

a = Analysis(
    [os.path.join(ROOT, 'deadband.py')],
    pathex=[ROOT],
    datas=[(os.path.join(ROOT, 'qml'), 'qml'),
           (os.path.join(ROOT, 'assets'), 'assets')],
    hiddenimports=HIDDEN,
    excludes=EXCLUDES,
    noarchive=False,
)
# PyInstaller collects every QML plugin PySide6 ships, which drags in Qt WebEngine
# (~150 MB) and friends that no Deadband page imports. Drop those files.
_UNUSED = ('webengine', 'webview', 'qt3d', 'quick3d', 'qtcharts', 'qt6charts',
           'datavisualization', 'qtgraphs', 'qt6graphs', 'qtlocation', 'qt6location',
           'qtpositioning', 'qt6positioning', 'qt6pdf', 'qtpdf', 'qtscxml', 'qt6scxml',
           'qtsensors', 'qt6sensors', 'qtremoteobjects', 'qt6remoteobjects',
           'qtvirtualkeyboard', 'qt6virtualkeyboard', 'qtwebchannel', 'qt6webchannel',
           'qtwebsockets', 'qt6websockets', 'qttest', 'qt6test', 'qtspatialaudio',
           'qt6spatialaudio', 'qtquicktimeline', 'qt6quicktimeline', 'qt6bluetooth',
           'qtbluetooth', 'qt6nfc', 'qtnfc', 'qt6serialport', 'qt6texttospeech',
           'qttexttospeech', 'qtwebview', 'qt6designer', 'qtdesigner')


def _used(entry):
    dest = entry[0].replace('\\', '/').lower()
    return not any(part in dest for part in _UNUSED)


a.binaries = [b for b in a.binaries if _used(b)]
a.datas = [d for d in a.datas if _used(d)]

pyz = PYZ(a.pure)

common = dict(debug=False, bootloader_ignore_signals=False, strip=False, upx=False,
              icon=ICON, exclude_binaries=True, version=None)
app = EXE(pyz, a.scripts, [], name='Deadband', console=False, **common)
cli = EXE(pyz, a.scripts, [], name='deadband-console', console=True, **common)

COLLECT(app, cli, a.binaries, a.datas, strip=False, upx=False, name='Deadband')
