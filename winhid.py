"""Windows HID specifics: collection paths, report capabilities, unit grouping.

On Linux one hidraw node carries a whole USB HID interface, every report id in
its descriptor. Windows splits an interface into one device path per TOP-LEVEL
COLLECTION, e.g. the Tarantula's interface 1 becomes four paths -- Consumer,
Keyboard, Mouse and the vendor collection (usage page 0xFFF0) that carries the
family command channel (out 0x0F, in 0x10 / 0x12). Two consequences:

* Only that vendor collection can carry the protocol, and Windows itself holds
  the keyboard and mouse collections open exclusively, so discovery must pick
  collections by usage page rather than try every path.
* WriteFile on a HID handle must be given exactly the collection's
  OutputReportByteLength. hidapi pads a SHORT buffer, but passes a longer one
  through and Windows then fails it. `caps()` reports the real lengths so the
  writer can fit its fixed 64-byte frames to them.

Everything here is read-only: CreateFile with zero access rights is enough to
query a collection's capabilities (it works even on the keyboard collections
Windows owns), and nothing is ever sent to the device. It uses only hid.dll and
kernel32, which ship with Windows -- no driver is installed or replaced.

The pure helpers (path parsing, grouping) work on any OS so they can be tested
anywhere; `caps()` returns None off Windows.
"""

import re
import sys

VENDOR_PAGE_MIN = 0xFF00

# \\?\HID#VID_3537&PID_103D&MI_01&Col04#8&2c3f5a1&0&0003#{4d1e55b2-f16f-11cf-88cb-001111000030}
_INSTANCE = re.compile(r'#([^#]+)#\{[0-9a-fA-F-]+\}\s*$')
_MI = re.compile(r'&MI_([0-9A-Fa-f]{2})', re.I)
_COL = re.compile(r'&Col([0-9A-Fa-f]{2})', re.I)


def _text(path):
    return path.decode(errors='replace') if isinstance(path, bytes) else (path or '')


def instance_id(path):
    """The device-instance part of a HID path ('8&2c3f5a1&0&0003'), or ''."""
    m = _INSTANCE.search(_text(path))
    return m.group(1) if m else ''


def unit_key(path):
    """A key shared by every collection of ONE physical interface, and different
    for a second identical controller.

    Windows numbers a composite interface's collections by the last field of the
    instance id (...&0000, ...&0001, ...); the part before it identifies the
    parent interface instance. Falls back to the whole path when the shape is
    unfamiliar (then each collection is simply its own group)."""
    inst = instance_id(path)
    if inst.count('&') >= 1:
        return inst.rsplit('&', 1)[0].lower()
    return _text(path).lower()


def interface_number(path):
    m = _MI.search(_text(path))
    return int(m.group(1), 16) if m else None


def collection_number(path):
    m = _COL.search(_text(path))
    return int(m.group(1), 16) if m else None


def is_vendor_page(usage_page):
    return (usage_page or 0) >= VENDOR_PAGE_MIN


# ---------------------------------------------------------------- capabilities
# HIDP_CAPS / HIDP_BUTTON_CAPS / HIDP_VALUE_CAPS from hidpi.h. Both *_CAPS
# structs are 72 bytes and start with USAGE UsagePage; UCHAR ReportID.
_CAPS_STRUCT_SIZE = 72
_HIDP_STATUS_SUCCESS = 0x00110000
_REPORT_TYPES = (('input', 0), ('output', 1), ('feature', 2))


def caps(path):
    """{usage_page, usage, input_len, output_len, feature_len,
        input_ids, output_ids, feature_ids} for one collection path, or None
    when it can't be queried (not Windows, path gone, ...). Lengths INCLUDE the
    report-id byte, as Windows defines them."""
    if sys.platform != 'win32':
        return None
    import ctypes
    from ctypes import wintypes

    class HIDP_CAPS(ctypes.Structure):
        _fields_ = [('Usage', wintypes.USHORT), ('UsagePage', wintypes.USHORT),
                    ('InputReportByteLength', wintypes.USHORT),
                    ('OutputReportByteLength', wintypes.USHORT),
                    ('FeatureReportByteLength', wintypes.USHORT),
                    ('Reserved', wintypes.USHORT * 17),
                    ('NumberLinkCollectionNodes', wintypes.USHORT),
                    ('NumberInputButtonCaps', wintypes.USHORT),
                    ('NumberInputValueCaps', wintypes.USHORT),
                    ('NumberInputDataIndices', wintypes.USHORT),
                    ('NumberOutputButtonCaps', wintypes.USHORT),
                    ('NumberOutputValueCaps', wintypes.USHORT),
                    ('NumberOutputDataIndices', wintypes.USHORT),
                    ('NumberFeatureButtonCaps', wintypes.USHORT),
                    ('NumberFeatureValueCaps', wintypes.USHORT),
                    ('NumberFeatureDataIndices', wintypes.USHORT)]

    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    hid = ctypes.WinDLL('hid')
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                wintypes.HANDLE]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    hid.HidD_GetPreparsedData.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
    hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
    hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]
    hid.HidP_GetCaps.restype = ctypes.c_long
    for fn in (hid.HidP_GetButtonCaps, hid.HidP_GetValueCaps):
        fn.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(wintypes.USHORT),
                       ctypes.c_void_p]
        fn.restype = ctypes.c_long

    FILE_SHARE_READ_WRITE, OPEN_EXISTING = 0x3, 3
    invalid = wintypes.HANDLE(-1).value
    handle = k32.CreateFileW(_text(path), 0, FILE_SHARE_READ_WRITE, None,
                             OPEN_EXISTING, 0, None)
    if not handle or handle == invalid:
        return None
    pre = ctypes.c_void_p()
    try:
        if not hid.HidD_GetPreparsedData(handle, ctypes.byref(pre)):
            return None
        try:
            c = HIDP_CAPS()
            if hid.HidP_GetCaps(pre, ctypes.byref(c)) & 0xFFFFFFFF != _HIDP_STATUS_SUCCESS:
                return None
            counts = {'input': (c.NumberInputButtonCaps, c.NumberInputValueCaps),
                      'output': (c.NumberOutputButtonCaps, c.NumberOutputValueCaps),
                      'feature': (c.NumberFeatureButtonCaps, c.NumberFeatureValueCaps)}
            out = {'usage_page': c.UsagePage, 'usage': c.Usage,
                   'input_len': c.InputReportByteLength,
                   'output_len': c.OutputReportByteLength,
                   'feature_len': c.FeatureReportByteLength}
            for name, rtype in _REPORT_TYPES:
                ids = set()
                for fn, n in zip((hid.HidP_GetButtonCaps, hid.HidP_GetValueCaps), counts[name]):
                    if not n:
                        continue
                    buf = ctypes.create_string_buffer(_CAPS_STRUCT_SIZE * n)
                    length = wintypes.USHORT(n)
                    if fn(rtype, buf, ctypes.byref(length), pre) & 0xFFFFFFFF == _HIDP_STATUS_SUCCESS:
                        raw = buf.raw
                        ids.update(raw[i * _CAPS_STRUCT_SIZE + 2] for i in range(length.value))
                out[f'{name}_ids'] = sorted(ids)
            return out
        finally:
            hid.HidD_FreePreparsedData(pre)
    finally:
        k32.CloseHandle(handle)


def describe(c):
    """One-line summary of caps() for diagnostics."""
    if not c:
        return 'capabilities unavailable'
    ids = lambda xs: ' '.join(f'{i:#04x}' for i in xs) or '-'
    return (f'page {c["usage_page"]:#06x} usage {c["usage"]:#06x}; '
            f'in {c["input_len"]}B [{ids(c.get("input_ids", []))}] '
            f'out {c["output_len"]}B [{ids(c.get("output_ids", []))}] '
            f'feature {c["feature_len"]}B')


def fit_output(buff, output_len):
    """Fit a write buffer to a collection's OutputReportByteLength.

    Deadband builds every command as a fixed 64-byte frame (report id + 63),
    zero-padded. Windows needs exactly `output_len` bytes: shorter is fine (hidapi
    pads), longer is refused. Trailing padding beyond the report is dropped; if
    anything NON-zero would be dropped the frame genuinely doesn't fit this
    collection, and that is an error rather than a silently truncated command."""
    data = bytes(bytearray(buff))
    if not output_len or len(data) <= output_len:
        return data
    if any(data[output_len:]):
        raise OSError(f'command of {len(data)} bytes does not fit this HID '
                      f'collection\'s {output_len}-byte output report')
    return data[:output_len]
