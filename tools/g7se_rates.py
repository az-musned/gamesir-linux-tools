"""Opt-in G7 SE rate validation using the production reader and Qt Save path.

Full snapshots and recovery files remain local. No captured authentication data
is emitted. Each run is bounded; a separate read-only mode checks a reconnect.
"""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import tempfile
import threading
import time

from vendors.gamesir.models.g7se import protocol as se
from vendors.gamesir.models.g7se.startup import Startup
import register_transaction


class WorkerStillRunning(OSError):
    """Do not open a second interface while an app worker may still own it."""


def identity(root):
    root = Path(root).resolve()
    result = {'sysfs': str(root)}
    for name in ('idVendor', 'idProduct', 'bcdDevice', 'busnum', 'devnum', 'serial'):
        if (root / name).exists():
            result[name] = (root / name).read_text().strip()
    if (result['idVendor'], result['idProduct'], result['bcdDevice']) != ('3537', '1010', '0630'):
        raise ValueError('Rate validation requires G7 SE descriptor 6.30')
    return result


def same_controller(expected, actual):
    # A reconnect changes devnum. With no serial, physical port + model/firmware
    # are the available identity: reconnect the same pad to the same USB port.
    return all(expected.get(key) == actual.get(key)
               for key in ('sysfs', 'idVendor', 'idProduct', 'bcdDevice', 'serial'))


def persist(path, report):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name, suffix='.pending', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(report, stream, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        if os.name == 'posix':          # directory fsync is POSIX-only
            directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


@contextmanager
def channel(root, expected):
    current = identity(root)
    if not same_controller(expected, current):
        raise OSError('Selected controller changed; rate operation refused')
    handle = se.open_device(int(current['busnum']), int(current['devnum']), root)
    startup = None
    try:
        startup = Startup(handle, int(current['busnum']), int(current['devnum']), root)
        startup.run()
        def read(bank, address, length):
            startup.check()
            startup.sequence = startup.sequence % 255 + 1
            seq = startup.sequence
            request = se.nexus_request(seq, 5, bytes((4, bank)) + address.to_bytes(2, 'big') + bytes((length,)))
            if handle.write(request) != len(request):
                raise OSError('Short profile read request')
            result = startup.receive(handle, [], lambda r: se.match_read(r, bank, address, length)
                if len(r) > 2 and r[2] == seq else None, seconds=1)
            if result is None:
                raise OSError('Profile read timed out')
            return result
        def write(bank, address, data):
            startup.check()
            if address != se.POLL_RATE:
                raise ValueError('Rate restoration cannot write other settings')
            startup.sequence = startup.sequence % 255 + 1
            wire = se.rate_write(startup.sequence, bank, data)
            ok = handle.write(wire) == len(wire)
            time.sleep(.03)
            return ok
        yield read, write
    finally:
        try:
            if startup is not None:
                startup.restore_input()
        finally:
            handle.close()


def snapshots(root, expected):
    with channel(root, expected) as (read, _write):
        return {str(bank): b''.join(read(bank, offset, min(55, se.PROFILE_SIZE-offset))
                for offset in range(0, se.PROFILE_SIZE, 55)).hex() for bank in se.RATE_BANKS}


def rate_values(snapshot):
    result = {}
    for bank in se.RATE_BANKS:
        data = bytes.fromhex(snapshot[str(bank)])
        if len(data) != se.PROFILE_SIZE:
            raise ValueError('Incomplete original profile snapshot')
        result[bank] = se.decode_rate(data[se.POLL_RATE:se.POLL_RATE+1])
    return result


def expected_profiles(baseline, code):
    result = {}
    for bank, raw in baseline.items():
        data = bytearray.fromhex(raw)
        data[se.POLL_RATE] = code
        result[bank] = data.hex()
    return result


def app_save(root, expected, code, recovery_dir):
    """Real reader, bridge staging and transactional Save; no mock transport."""
    os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
    from PySide6.QtCore import QTimer
    from PySide6.QtGui import QGuiApplication
    from bridge import GamesirBridge
    import controller_profile as profiles
    from gs_common import find_controllers
    from gs_state import state
    import reader

    if not same_controller(expected, identity(root)):
        raise OSError('Selected controller changed')
    ctrl = next(c for c in find_controllers() if c.get('usb')
                and Path(c['usb']['sysfs']).resolve() == Path(root).resolve())
    app = QGuiApplication.instance() or QGuiApplication([])
    profiles.set_active(profiles.G7_SE)
    state.update(selected=ctrl['id'], driving=None, connected=True, config_wanted=True,
                 config_claimed=False, demo=False, profile=None, edit_profile=None,
                 controller=profiles.G7_SE.short, usb_bcd=se.TESTED_DESCRIPTOR)
    bridge = GamesirBridge()
    for timer in bridge.findChildren(QTimer):
        timer.stop()
    deadline = time.monotonic() + 40
    def wait(predicate):
        while time.monotonic() < deadline:
            app.processEvents()
            bridge._poll_status()
            if not bridge._backup_busy:
                bridge._poll_config()
                bridge._poll_se_rates()
            if predicate():
                return
            if not worker.is_alive():
                raise OSError('Reader stopped: ' + state.get('config_status', ''))
            time.sleep(.02)
        raise OSError('App rate validation timed out: ' + state.get('config_status', ''))

    old_data_home = os.environ.get('XDG_DATA_HOME')
    os.environ['XDG_DATA_HOME'] = str(recovery_dir)
    worker = threading.Thread(target=reader.read_session_seusb, args=(ctrl,), daemon=True)
    try:
        worker.start()
        wait(lambda: bridge.pollRateEditable)
        bridge.setPoll(code)
        if bridge.pendingCount == 0 and bridge.config.get('poll') == code:
            return 'Already configured; all four banks read back'
        if bridge.pendingCount != 1:
            raise OSError('App refused to stage exactly one rate change')
        bridge.applyConfig()
        wait(lambda: not bridge._backup_busy)
        status = bridge._apply_status
        if not status.startswith('Applied') or bridge.pendingCount:
            raise OSError('App rate Save failed: ' + status)
        return status
    finally:
        state['config_wanted'] = False
        worker.join(3)
        if worker.is_alive():
            reader.release_controller()
            worker.join(2)
        # Give a save worker time to observe the closed generation and finish.
        for _ in range(100):
            if not bridge._backup_busy:
                break
            app.processEvents()
            time.sleep(.02)
        stopped = not bridge._backup_busy and not worker.is_alive()
        if stopped:
            bridge.deleteLater()
        if old_data_home is None:
            os.environ.pop('XDG_DATA_HOME', None)
        else:
            os.environ['XDG_DATA_HOME'] = old_data_home
        if not stopped:
            raise WorkerStillRunning('App worker did not stop; manual recovery required')


def restore(root, report, recovery_dir):
    originals = rate_values(report['baseline'])
    with channel(root, report['identity']) as (read, write):
        def checked_read(bank, addr, length):
            value = read(bank, addr, length)
            se.decode_rate(value)
            return value
        ok, message = register_transaction.apply(
            [(bank, se.POLL_RATE, [code]) for bank, code in originals.items()],
            checked_read, write, str(recovery_dir), 'GameSir G7 SE report-rate restoration')
        if not ok:
            raise OSError(message)
    return snapshots(root, report['identity']) == report['baseline']


def run(root, output, previous=None, restore_only=False):
    root, output = Path(root).resolve(), Path(output).resolve()
    if output.exists():
        raise ValueError('Choose a new output path; never overwrite a recovery baseline')
    recovery_dir = output.parent / (output.stem + '-recovery')
    if previous:
        baseline = json.loads(Path(previous).read_text())
        rate_values(baseline['baseline'])
        current = identity(root)
        if not same_controller(baseline['identity'], current):
            raise OSError('Reconnect the original controller to the same USB port')
        if not restore_only and current['devnum'] == baseline['identity']['devnum']:
            raise OSError('Controller has not been unplugged/reconnected')
        report = {'identity': current, 'source': str(previous), 'mode': 'restore' if restore_only else 'reconnect-read-only'}
        if restore_only:
            report['success'] = restore(root, baseline, recovery_dir)
        else:
            report['profiles'] = snapshots(root, baseline['identity'])
            report['success'] = report['profiles'] == baseline['baseline']
        persist(output, report)
        return report['success']
    report = {'identity': identity(root), 'mode': 'app-rate-roundtrip', 'operations': [], 'success': False}
    attempted = False
    try:
        report['baseline'] = snapshots(root, report['identity'])
        rate_values(report['baseline'])
        if snapshots(root, report['identity']) != report['baseline']:
            raise OSError('Original profiles did not read consistently; nothing written')
        persist(output, report)  # durable full originals before the first Save
        originals = rate_values(report['baseline'])
        # Begin at a different setting so every code exercises an actual Save.
        codes = [0, 1, 2] if set(originals.values()) != {0} else [1, 2, 0]
        for code in codes:
            attempted = True
            status = app_save(root, report['identity'], code, recovery_dir)
            after = snapshots(root, report['identity'])
            verified = after == expected_profiles(report['baseline'], code)
            report['operations'].append({'code': code, 'rate': se.POLL_RATES[code],
                'status': status, 'only_rate_bytes_changed': verified})
            persist(output, report)
            if not verified:
                raise OSError('Rate or unrelated profile bytes differed; stopping')
    except WorkerStillRunning as exc:
        report['error'] = str(exc)
        report['restoration_error'] = 'Recovery deferred until the app worker has exited'
        attempted = False
    except Exception as exc:
        report['error'] = str(exc)
    finally:
        if attempted:
            try:
                report['originals_restored'] = restore(root, report, recovery_dir)
            except Exception as exc:
                report['restoration_error'] = str(exc)
        report['success'] = (not report.get('error') and len(report['operations']) == 3
                             and report.get('originals_restored', False))
        persist(output, report)
    return report['success']
