"""Shared test setup.

The write-approval gate (write_gate.py) is on by default on Windows. The suite
exercises the app's write paths against fake devices, which is exactly what
the gate exists to stop on real hardware, so it is switched off here; the
gate's own tests (test_windows_port.py) switch it back on explicitly.
"""
import os

os.environ.setdefault('DEADBAND_WRITE_APPROVAL', '0')

import pytest  # noqa: E402

import hostos  # noqa: E402

# Tests of the G7 SE's raw-USB configuration path, which only Linux has
# (hostos.RAW_USB_CONFIG). Off Linux the reader deliberately leaves the G7 SE
# input-only and says so, so a test that drives read_controller() into the SE
# session never reaches it (it waits for ever), and the SE's authentication
# shells out to the system `openssl`, which Windows doesn't ship. Each is named
# so the list can't quietly hide a new failure.
_LINUX_ONLY = {
    'tests/test_g7se_auth.py::AuthTests::test_synthetic_public_key_can_encrypt_fresh_secret_with_openssl':
        'G7 SE authentication uses the system openssl binary',
    'tests/test_g7se_auth.py::AuthTests::test_wrong_fresh_transcript_prevents_completion':
        'G7 SE authentication uses the system openssl binary',
    'tests/test_g7se_app.py::SeAppTests::test_reader_preserves_failures_across_input_only_iterations_until_retry':
        'the reader configures the G7 SE over raw USB on Linux only',
    'tests/test_g7se_app.py::SeAppTests::test_reader_drops_previous_devices_error_and_publishes_descriptor':
        'the reader configures the G7 SE over raw USB on Linux only',
    'tests/test_g7se_app.py::SeAppTests::test_unsupported_firmware_stays_input_only':
        'the reader configures the G7 SE over raw USB on Linux only',
}


def pytest_collection_modifyitems(config, items):
    if hostos.RAW_USB_CONFIG:
        return
    for item in items:
        reason = _LINUX_ONLY.get(item.nodeid.replace('\\', '/'))
        if reason:
            item.add_marker(pytest.mark.skip(reason=f'Linux only: {reason}'))
