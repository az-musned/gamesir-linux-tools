"""One interface over the two Python HID libraries that both install as `hid`.

Deadband is written against **cython-hidapi** (PyPI `hidapi`, Arch
`python-hidapi`): ``hid.device()`` + ``open_path()``, ``read()`` returning a list
of ints, ``set_nonblocking()``.

**pyhidapi** (PyPI `hid`, Arch `python-hid`) is a different, ctypes-based library
with a different API -- ``hid.Device(path=...)``, ``read()`` returning bytes, a
``nonblocking`` property, ``HIDException`` -- and it ALSO installs as ``import
hid``. The two conflict at the package level, and SteamOS ships pyhidapi because
``jupiter-hw-support`` (the Steam Deck's own hardware support) depends on it. So
on a Steam Deck the cython one cannot be installed without removing the Deck's
hardware support, and building it from source needs a compiler and headers that
SteamOS doesn't have (issues #13, #19).

Rather than fight that, the app imports this module instead of ``hid``. When
cython-hidapi is installed it is passed straight through, untouched. When only
pyhidapi is, a thin adapter gives it the cython-hidapi shape. pyhidapi loads the
system ``libhidapi-hidraw`` first, so it gets the hidraw backend the app needs.
"""
import gc as _gc
import sys as _sys

_WINDOWS = _sys.platform == 'win32'


def _output_len(path):
    """Windows only: the collection's OutputReportByteLength (see winhid), so a
    fixed 64-byte frame can be fitted to it; None elsewhere or when unknown."""
    if not _WINDOWS:
        return None
    try:
        import winhid
        c = winhid.caps(path)
        return c['output_len'] if c else None
    except Exception:
        return None


def _fit(buff, out_len):
    if not out_len:
        return buff
    import winhid
    return list(winhid.fit_output(buff, out_len))

try:
    # cython-hidapi built with BOTH backends (some distro packages) installs the
    # hidraw one as `hidraw` and keeps `hid` for libusb, which can't open
    # /dev/hidraw paths -- so prefer `hidraw` when it's there (#36, thanks
    # @Barracuda72). Same API as `hid`; absent on single-backend builds.
    import hidraw as _hid
except ImportError:
    import hid as _hid

if hasattr(_hid, 'device'):
    # cython-hidapi: the API the app is written against. Disable atexit on its
    # internal weakref.finalize so interpreter shutdown never calls hid_close()
    # on the main thread while a daemon reader thread is inside hid_read_timeout().
    IMPL = 'hidapi (cython)'
    enumerate = _hid.enumerate
    __version__ = getattr(_hid, '__version__', '?')

    class device(_hid.device):
        # A `usb:` path is a HID interface with no system driver (a Cyclone
        # dongle on macOS): rawhid opens it over libusb and every call below
        # goes there instead.
        _raw = None
        _out_len = None

        def open_path(self, path):
            if (path.decode() if isinstance(path, bytes) else path).startswith('usb:'):
                import rawhid
                raw = rawhid.Device()
                raw.open_path(path)
                self._raw = raw
                return
            super().open_path(path)
            self._out_len = _output_len(path)
            for ref in _gc.get_referents(self):
                if hasattr(ref, 'atexit'):
                    ref.atexit = False

        def read(self, max_length, timeout_ms=0):
            if self._raw is not None:
                return self._raw.read(max_length, timeout_ms)
            return super().read(max_length, timeout_ms)

        def write(self, buff):
            if self._raw is not None:
                return self._raw.write(buff)
            return super().write(_fit(buff, self._out_len))

        def set_nonblocking(self, v):
            if self._raw is not None:
                return self._raw.set_nonblocking(v)
            return super().set_nonblocking(v)

        def get_product_string(self):
            if self._raw is not None:
                return self._raw.get_product_string()
            return super().get_product_string()

        def close(self):
            if self._raw is not None:
                self._raw.close()
                return
            super().close()

        def open(self, vendor_id=0, product_id=0, serial_number=None):
            super().open(vendor_id, product_id, serial_number)
            for ref in _gc.get_referents(self):
                if hasattr(ref, 'atexit'):
                    ref.atexit = False

else:
    IMPL = 'python-hid (ctypes)'
    __version__ = getattr(_hid, '__version__', '?')

    def enumerate(vendor_id=0, product_id=0):
        # Same dict keys as cython-hidapi (path, vendor_id, product_id,
        # usage_page, interface_number, ...), and path is bytes in both.
        return _hid.enumerate(vendor_id, product_id)

    class device:
        """cython-hidapi's ``hid.device`` interface over a pyhidapi Device.

        Errors are raised as OSError, as cython-hidapi does, so callers that
        catch OSError (or Exception) behave the same on either library."""

        def __init__(self):
            self._d = None
            self._nonblocking = False
            self._out_len = None

        def _opened(self, **kw):
            try:
                self._d = _hid.Device(**kw)
            except _hid.HIDException as e:
                raise OSError(str(e) or 'open failed') from e
            if self._nonblocking:
                self._d.nonblocking = True

        def open_path(self, path):
            if isinstance(path, str):
                path = path.encode()
            self._opened(path=path)
            self._out_len = _output_len(path)

        def open(self, vendor_id=0, product_id=0, serial_number=None):
            self._opened(vid=vendor_id, pid=product_id, serial=serial_number)

        def set_nonblocking(self, v):
            self._nonblocking = bool(v)
            if self._d is not None:
                self._d.nonblocking = bool(v)
            return 0

        def read(self, max_length, timeout_ms=0):
            # cython-hidapi: timeout_ms == 0 means plain hid_read (which honours
            # the non-blocking flag); otherwise hid_read_timeout. pyhidapi maps
            # timeout=None to hid_read the same way.
            try:
                data = self._d.read(max_length, timeout_ms if timeout_ms else None)
            except _hid.HIDException as e:
                raise OSError(str(e) or 'read error') from e
            return list(data or b'')        # cython-hidapi returns a list of ints

        def write(self, buff):
            try:
                return self._d.write(bytes(bytearray(_fit(buff, self._out_len))))
            except _hid.HIDException as e:
                raise OSError(str(e) or 'write error') from e

        def get_feature_report(self, report_id, max_length):
            try:
                return list(self._d.get_feature_report(report_id, max_length))
            except _hid.HIDException as e:
                raise OSError(str(e) or 'get_feature_report error') from e

        def send_feature_report(self, buff):
            try:
                return self._d.send_feature_report(bytes(bytearray(buff)))
            except _hid.HIDException as e:
                raise OSError(str(e) or 'send_feature_report error') from e

        def get_product_string(self):
            return self._d.product

        def get_manufacturer_string(self):
            return self._d.manufacturer

        def get_serial_number_string(self):
            return self._d.serial

        def close(self):
            if self._d is not None:
                try:
                    self._d.close()
                finally:
                    self._d = None
