"""A simulated Tarantula Pro 8K / T3CE for testing the Windows path without hardware.

It stands in for the HID library (hidcompat.enumerate / hidcompat.device), and
presents itself exactly as Windows does: one USB interface split into four
top-level collections (Consumer, Keyboard, Mouse, and the vendor 0xFFF0
collection that carries the family channel: out 0x0F, in 0x10 / 0x12, 64-byte
reports). Behind it is a small model of the pad: five profile banks plus the
lighting bank, profile switching (a bank only answers while its profile is
active, as on the real pad), register reads and writes, and a live 0x12 input
stream with moving sticks.

    python tools/sim_tarantula.py              # the full app, driven by the simulated pad
    python tools/sim_tarantula.py --selftest   # headless end-to-end check, exit code 0 = pass

The self-test runs the REAL discovery (Windows collection handling), reader,
write gate, backup walk and register writes against the simulator and checks
every frame the app sends. It proves the software path, not the hardware: the
real pad's behaviour is only what the project's captures documented.
"""
import math
import os
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

GUID = '{4d1e55b2-f16f-11cf-88cb-001111000030}'
PID, BCD = 0x103D, 0x0249
PRODUCT = 'Tarantula 8K PC (simulated)'
VENDOR_PATH = f'\\\\?\\HID#VID_3537&PID_103D&MI_01&Col04#9&51ab&0&0003#{GUID}'
OUTPUT_LEN = 64
LOG_WRITES = False            # the GUI mode prints every register write it receives


class Pad:
    """The simulated controller's memory and protocol."""

    def __init__(self):
        import controller_profile as profiles
        prof = profiles.TARANTULA_PRO_8K
        self.prof = prof
        self.lock = threading.Lock()
        self.profile = 1
        self.banks = {b: bytearray(0x1000) for b in (1, 2, 3, 4, 5, 0x20)}
        for b in (1, 2, 3, 4):
            mem = self.banks[b]
            mem[prof.POLL_RATE] = 2                                  # 1000 Hz
            for lo, hi in ((prof.LT_DZ_MIN, prof.LT_DZ_MAX),
                           (prof.LT_DZ_MIN + prof.RT_OFFSET, prof.LT_DZ_MAX + prof.RT_OFFSET),
                           (prof.ST_DZ_MIN, prof.ST_DZ_MAX),
                           (prof.ST_DZ_MIN + prof.RS_OFFSET, prof.ST_DZ_MAX + prof.RS_OFFSET)):
                mem[lo:lo + 2] = (50).to_bytes(2, 'big')             # 5.0 %
                mem[hi:hi + 2] = (1000).to_bytes(2, 'big')           # 100.0 %
            mem[prof.ST_DZ_MIN:prof.ST_DZ_MIN + 2] = (50 + b).to_bytes(2, 'big')   # bank tag
        self.replies = []
        self.frames = []           # every frame the app wrote, for the self-test
        self.t0 = time.time()
        self.last_stream = 0.0

    def bank_open(self, bank):
        return bank not in (1, 2, 3, 4, 5) or bank == self.profile

    def write(self, frame):
        f = list(frame)
        with self.lock:
            self.frames.append(f)
            if not f or f[0] != 0x0F:
                return
            op = f[1]
            if op == 0x0B:
                self.replies.append([0x10, 0x0C, self.profile])
            elif op == 0x07 and f[2] in (1, 2, 3, 4):
                self.profile = f[2]
            elif op == 0x09:
                self.replies.append([0x10, 0x0A, BCD >> 8, BCD & 0xFF])
            elif op == 0x04:
                bank, addr, ln = f[2], (f[3] << 8) | f[4], f[5]
                if bank in self.banks and self.bank_open(bank):
                    data = list(self.banks[bank][addr:addr + ln])
                    self.replies.append([0x10, 0x05, bank, f[3], f[4], ln] + data)
            elif op == 0x03:
                bank, addr, ln = f[2], (f[3] << 8) | f[4], f[5]
                if LOG_WRITES:
                    print(f'[sim] write bank {bank} @0x{addr:04x}: '
                          + ' '.join(f'{x:02x}' for x in f[6:6 + ln]), flush=True)
                if bank in self.banks and self.bank_open(bank):
                    self.banks[bank][addr:addr + ln] = bytes(f[6:6 + ln])

    def read(self):
        with self.lock:
            if self.replies:
                r = self.replies.pop(0)
                return r + [0] * (64 - len(r))
        now = time.time()
        if now - self.last_stream < 0.008:
            return []
        self.last_stream = now
        t = now - self.t0
        r = [0] * 64
        r[0] = 0x12
        r[1] = int(128 + 90 * math.cos(t))
        r[2] = int(128 + 90 * math.sin(t))
        r[3], r[4] = 128, int(128 + 60 * math.sin(t * 0.7))
        r[5] = 0x0F | (0x20 if int(t) % 3 == 0 else 0)           # dpad neutral, A pulses
        r[8] = int(127 + 127 * math.sin(t * 1.3))                 # LT
        r[35], r[36] = 0x01, 90                                   # cable, battery
        return r


PAD = None


def enumerate_(vendor_id=0, product_id=0):
    cols = [(0x000C, 0x0001), (0x0001, 0x0006), (0x0001, 0x0002), (0xFFF0, 0x0001)]
    out = []
    for i, (page, usage) in enumerate(cols):
        path = VENDOR_PATH.replace('Col04', f'Col{i + 1:02d}').replace('&0003#', f'&{i:04d}#')
        out.append({'path': path.encode(), 'vendor_id': 0x3537, 'product_id': PID,
                    'release_number': BCD, 'product_string': PRODUCT,
                    'manufacturer_string': 'GameSir', 'serial_number': '',
                    'usage_page': page, 'usage': usage, 'interface_number': 1})
    if vendor_id and vendor_id != 0x3537:
        return []
    return out


class Device:
    """hidcompat.device's shape over the simulated pad."""

    def __init__(self):
        self.open_ = False
        self.lengths = []

    def open_path(self, path):
        p = path.decode() if isinstance(path, bytes) else path
        if p != VENDOR_PATH:
            # Windows holds keyboard / mouse collections exclusively
            raise OSError('open failed (access denied)')
        self.open_ = True

    def set_nonblocking(self, v):
        return 0

    def read(self, max_length, timeout_ms=0):
        end = time.time() + timeout_ms / 1000.0
        while True:
            r = PAD.read()
            if r or time.time() >= end:
                return r[:max_length]
            time.sleep(0.002)

    def write(self, buff):
        import winhid
        data = winhid.fit_output(buff, OUTPUT_LEN)          # what Windows would accept
        self.lengths.append(len(data))
        PAD.write(data)
        return len(data)

    def get_product_string(self):
        return PRODUCT

    def close(self):
        self.open_ = False


def install():
    """Route the app's HID access, and its discovery, to the simulator."""
    global PAD
    import hidcompat
    import gs_common
    import hostos
    PAD = Pad()
    hidcompat.enumerate = enumerate_
    hidcompat.device = Device
    hidcompat.IMPL = 'simulated Tarantula'
    hostos.PER_COLLECTION_HID_PATHS = True          # Windows-shaped discovery on any OS
    gs_common.find_controllers = gs_common._find_controllers_portable
    import reader
    reader.find_controllers = gs_common._find_controllers_portable
    return PAD


# ----------------------------------------------------------------------- selftest
def _wait(cond, secs, what):
    end = time.time() + secs
    while time.time() < end:
        if cond():
            return
        time.sleep(0.05)
    raise AssertionError(f'timed out waiting for: {what}')


def selftest():
    tmp = tempfile.mkdtemp(prefix='deadband-sim-')
    os.environ['XDG_DATA_HOME'] = tmp
    pad = install()
    import hostos
    import write_gate
    import backup
    import reader
    from gs_state import state
    from vendors.gamesir import control
    hostos.REQUIRES_WRITE_APPROVAL = True
    write_gate._path_override = os.path.join(tmp, 'approvals.json')
    prof = pad.prof
    checks = []

    def ok(msg):
        checks.append(msg)
        print('  ok  ' + msg, flush=True)

    # read-only register dump + diff (before the reader owns the pad)
    import doctor
    from vendors.gamesir import regdump
    a_path, b_path = os.path.join(tmp, 'before.json'), os.path.join(tmp, 'after.json')
    assert doctor.dump_registers(a_path) == 0
    l4 = dict(prof.MACRO_SLOTS)['L4']
    pad.banks[1][l4 + 6] = 0x01             # what "loop" might change in GameSir Connect
    assert doctor.dump_registers(b_path) == 0
    assert not any(f[1] in (0x03, 0x07) for f in pad.frames), 'the dump wrote or switched'
    rows = regdump.diff(regdump.load(a_path), regdump.load(b_path), prof)
    assert [(r[1], r[3], r[4]) for r in rows] == [(l4 + 6, 1, 'L4 block +0x06 (UNKNOWN +6)')], rows
    pad.banks[1][l4 + 6] = 0x00
    ok('read-only register dump + diff found the one changed byte: ' + rows[0][4])
    pad.frames.clear()

    threading.Thread(target=reader.read_controller, daemon=True).start()
    _wait(lambda: state.get('driving') and state.get('mode_ok') and state.get('profile'),
          15, 'the reader to open the vendor collection and stream')
    ok(f'discovered and opened {state["driving"]}')
    assert state['controller'] == 'Tarantula Pro 8K', state['controller']
    assert state['firmware'] == '2.49', state['firmware']
    ok('identified as Tarantula Pro 8K, firmware 2.49')
    assert 30 <= state['lx'] <= 230 and state['battery'] == 90
    ok(f'live input: lx={state["lx"]} ly={state["ly"]} battery={state["battery"]}%')
    assert write_gate.status() == write_gate.LOCKED
    ok('writes locked: ' + write_gate.describe())

    time.sleep(1.0)
    ops = {f[1] for f in pad.frames if f and f[0] == 0x0F}
    assert ops <= {0xF2, 0x0B, 0x04, 0x09}, sorted(hex(o) for o in ops)
    ok('before approval only heartbeat/queries/reads were sent: '
       + ' '.join(f'{o:02x}' for o in sorted(ops)))

    before = bytes(pad.banks[1])
    assert not control.write_reg(1, prof.POLL_RATE, [4])
    assert not control.set_profile(2)
    assert bytes(pad.banks[1]) == before and pad.profile == 1
    assert not any(f[1] in (0x03, 0x07) for f in pad.frames)
    ok('locked: a register write and a profile switch were refused before reaching the pad')

    # the approval flow the banner runs: full backup first, then unlock
    ident = write_gate.identity()
    assert write_gate.begin_backup()
    path = os.path.join(write_gate.backup_dir(), 'before-first-write.json')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    result = {}
    backup.export_async(path, on_done=lambda good, msg: result.update(ok=good, msg=msg))
    _wait(lambda: 'ok' in result, 120, 'the pre-approval backup')
    assert result['ok'], result['msg']
    assert not any(f[1] == 0x03 for f in pad.frames), 'a write during the backup'
    assert pad.profile == 1, 'pad not returned to its profile'
    import json
    with open(path) as fh:
        snap = json.load(fh)
    assert sorted(snap['profiles']) == ['1', '2', '3', '4'], list(snap['profiles'])
    for b in (1, 2, 3, 4):              # each bank's tag proves it was read from THAT profile
        entry = next(e for e in snap['profiles'][str(b)].values()
                     if e['addr'] == f'0x{prof.ST_DZ_MIN:04x}')
        assert entry['bytes'] == [0, 50 + b], (b, entry)
    ok('backup walked profiles 1-4 (only 0x07 switches, no writes), each bank verified, '
       'and returned to profile 1')
    assert write_gate.finish_backup(True, path, ident)
    assert write_gate.status() == write_gate.APPROVED
    ok('approved for exactly this identity, persisted to ' + write_gate.store_path())

    gen = control.generation()
    new = (73).to_bytes(2, 'big')                                   # 7.3 %
    assert control.write_reg(1, prof.ST_DZ_MIN, list(new), gen=gen)
    _wait(lambda: bytes(pad.banks[1][prof.ST_DZ_MIN:prof.ST_DZ_MIN + 2]) == new, 3, 'the write')
    back = control.read_reg_sync(1, prof.ST_DZ_MIN, 2, gen=gen)
    assert back is not None and bytes(back) == new, back
    ok('approved write landed and read back identically (left stick deadzone 5.1% -> 7.3%)')
    assert all(len(f) == OUTPUT_LEN for f in pad.frames), {len(f) for f in pad.frames}
    ok(f'every frame fitted the collection\'s {OUTPUT_LEN}-byte output report '
       f'({len(pad.frames)} frames)')
    print(f'RESULT: PASS ({len(checks)} checks)')
    return 0


def gui():
    global LOG_WRITES
    LOG_WRITES = True
    install()
    sys.argv = [sys.argv[0]] + [a for a in sys.argv[1:] if a != '--gui']
    import runpy
    runpy.run_path(os.path.join(ROOT, 'deadband.py'), run_name='__main__')


if __name__ == '__main__':
    if '--selftest' in sys.argv:
        try:
            raise SystemExit(selftest())
        except AssertionError as e:
            print(f'RESULT: FAIL ({e})')
            raise SystemExit(1)
    gui()
