"""The Windows port: HID collection handling, discovery, the write-approval
gate, and the read-only register-map check. Runs on every OS (the Windows
behaviour is exercised through its pure helpers and fakes); a few checks that
need the real Windows API run only there."""
import json
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

import controller_profile as profiles
import gs_common
import hostos
import winhid
import write_gate
from vendors.gamesir import control

GUID = '{4d1e55b2-f16f-11cf-88cb-001111000030}'


def path(instance, col, mi=1, pid='103D'):
    return (f'\\\\?\\HID#VID_3537&PID_{pid}&MI_{mi:02d}&Col{col:02d}#{instance}#{GUID}').encode()


def tarantula_entries(prefix='8&2c3f5a1&0', bcd=0x0249, pid=0x103D):
    """What hid.enumerate() returns on Windows for a Tarantula in PC mode: the
    XInput interface 0 isn't HID, interface 1 splits into four collections."""
    cols = [(0x000C, 0x0001), (0x0001, 0x0006), (0x0001, 0x0002), (0xFFF0, 0x0001)]
    return [{'path': path(f'{prefix}&{i:04d}', i + 1, pid=f'{pid:04X}'),
             'vendor_id': 0x3537, 'product_id': pid, 'release_number': bcd,
             'product_string': 'Tarantula 8K PC', 'serial_number': '',
             'usage_page': page, 'usage': usage, 'interface_number': 1}
            for i, (page, usage) in enumerate(cols)]


class PathParsing(unittest.TestCase):
    def test_instance_and_unit_key(self):
        p = path('8&2c3f5a1&0&0003', 4)
        self.assertEqual(winhid.instance_id(p), '8&2c3f5a1&0&0003')
        self.assertEqual(winhid.unit_key(p), '8&2c3f5a1&0')
        self.assertEqual(winhid.interface_number(p), 1)
        self.assertEqual(winhid.collection_number(p), 4)

    def test_collections_of_one_interface_share_a_key(self):
        keys = {winhid.unit_key(e['path']) for e in tarantula_entries()}
        self.assertEqual(len(keys), 1)

    def test_unfamiliar_path_is_its_own_group(self):
        self.assertEqual(winhid.unit_key(b'/dev/hidraw3'), '/dev/hidraw3')
        self.assertIsNone(winhid.interface_number(b'/dev/hidraw3'))


class OutputFitting(unittest.TestCase):
    def test_short_and_exact_frames_pass(self):
        self.assertEqual(winhid.fit_output([0x0F, 1, 2], 64), bytes([0x0F, 1, 2]))
        frame = gs_common.pad(0x0F, 0xF2)
        self.assertEqual(winhid.fit_output(frame, 64), bytes(frame))

    def test_zero_padding_beyond_the_report_is_dropped(self):
        frame = gs_common.pad(0x0F, 0x04, 1, 0, 0x2E, 1)       # 64 bytes
        self.assertEqual(winhid.fit_output(frame, 33), bytes(frame[:33]))

    def test_real_data_beyond_the_report_is_an_error(self):
        frame = [0x0F] + [0x11] * 63
        with self.assertRaises(OSError):
            winhid.fit_output(frame, 33)

    def test_unknown_length_passes_through(self):
        self.assertEqual(winhid.fit_output([1, 2, 3], None), bytes([1, 2, 3]))


class WindowsDiscovery(unittest.TestCase):
    def test_one_unit_with_only_the_vendor_collection_offered(self):
        [c] = gs_common._find_controllers_windows(tarantula_entries())
        self.assertEqual(c['pid'], 0x103D)
        self.assertEqual(c['bcd'], 0x0249)
        self.assertEqual(c['product'], 'Tarantula 8K PC')
        self.assertEqual(len(c['collections']), 4)
        self.assertEqual(c['nodes'], [path('8&2c3f5a1&0&0003', 4).decode()])
        self.assertEqual(c['interface'], 1)
        self.assertIs(profiles.detect_one(c['pid'], c['product']), profiles.TARANTULA_PRO_8K)

    def test_two_identical_pads_stay_apart(self):
        found = gs_common._find_controllers_windows(
            tarantula_entries('8&aaaa&0') + tarantula_entries('8&bbbb&0'))
        self.assertEqual(len(found), 2)
        self.assertNotEqual(found[0]['id'], found[1]['id'])
        self.assertTrue(all(len(c['nodes']) == 1 for c in found))

    def test_id_is_stable_across_scans(self):
        a = gs_common._find_controllers_windows(tarantula_entries())
        b = gs_common._find_controllers_windows(list(reversed(tarantula_entries())))
        self.assertEqual([c['id'] for c in a], [c['id'] for c in b])

    def test_other_vendors_are_ignored(self):
        other = dict(tarantula_entries()[0], vendor_id=0x046D)
        self.assertEqual(gs_common._find_controllers_windows([other]), [])

    def test_no_vendor_collection_lists_the_pad_without_a_channel(self):
        entries = [e for e in tarantula_entries(pid=0x103C) if e['usage_page'] < 0xFF00]
        [c] = gs_common._find_controllers_windows(entries)
        self.assertEqual(c['nodes'], [])


class FakeDevice:
    def __init__(self):
        self.writes = []

    def write(self, data):
        self.writes.append(list(data))
        return len(data)


class WriteGate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.p1 = patch.object(hostos, 'REQUIRES_WRITE_APPROVAL', True)
        self.p2 = patch.object(write_gate, '_path_override',
                               os.path.join(self.tmp.name, 'approvals.json'))
        self.p1.start(); self.p2.start()
        self.addCleanup(self.p1.stop); self.addCleanup(self.p2.stop)
        write_gate.reload()
        write_gate.bind(None)
        self.ident = write_gate.make_identity(0x3537, 0x103D, 'Tarantula 8K PC', 0x0249,
                                              'Tarantula Pro 8K')
        profiles.set_active(profiles.TARANTULA_PRO_8K)
        self.addCleanup(profiles.set_active, None)
        self.dev = FakeDevice()
        control.set_device(self.dev)
        self.addCleanup(control.clear_device)
        self.addCleanup(write_gate.bind, None)

    def test_identity_text(self):
        self.assertEqual(self.ident['firmware'], '2.49')
        self.assertIn('3537:103d', write_gate.describe(self.ident))
        self.assertIn('Tarantula 8K PC', write_gate.describe(self.ident))

    def test_nothing_bound_is_locked(self):
        self.assertEqual(write_gate.status(), write_gate.LOCKED)

    def test_locked_refuses_writes_but_keeps_reads_and_heartbeat(self):
        write_gate.bind(self.ident)
        self.assertFalse(control.send_cmd(0x0F, 0x03, 1, 0x00, 0x2E, 1, 2))
        self.assertFalse(control.send_cmd(0x0F, 0x07, 2))          # profile select
        self.assertEqual(self.dev.writes, [])
        self.assertTrue(control.send_cmd(0x0F, 0xF2, probe=True))  # heartbeat
        self.assertTrue(control.send_cmd(0x0F, 0x04, 1, 0, 0x2E, 1, probe=True))
        self.assertEqual([w[:2] for w in self.dev.writes], [[0x0F, 0xF2], [0x0F, 0x04]])

    def test_backup_step_allows_only_profile_select(self):
        write_gate.bind(self.ident)
        self.assertTrue(write_gate.begin_backup())
        self.assertEqual(write_gate.status(), write_gate.BACKING_UP)
        self.assertTrue(control.send_cmd(0x0F, 0x07, 2))
        self.assertFalse(control.send_cmd(0x0F, 0x03, 1, 0x00, 0x2E, 1, 2))
        self.assertEqual([w[:3] for w in self.dev.writes], [[0x0F, 0x07, 2]])

    def test_successful_backup_approves_and_persists(self):
        write_gate.bind(self.ident)
        write_gate.begin_backup()
        self.assertTrue(write_gate.finish_backup(True, 'C:/b.json', self.ident))
        self.assertEqual(write_gate.status(), write_gate.APPROVED)
        self.assertTrue(control.send_cmd(0x0F, 0x03, 1, 0x00, 0x2E, 1, 2))
        with open(write_gate.store_path()) as fh:
            stored = json.load(fh)['approved']
        [record] = stored.values()
        self.assertEqual(record['backup'], 'C:/b.json')
        write_gate.reload()                       # a fresh launch reads it back
        self.assertEqual(write_gate.status(), write_gate.APPROVED)

    def test_failed_backup_stays_locked(self):
        write_gate.bind(self.ident)
        write_gate.begin_backup()
        self.assertFalse(write_gate.finish_backup(False, 'x', self.ident))
        self.assertEqual(write_gate.status(), write_gate.LOCKED)

    def test_controller_swapped_during_backup_is_not_approved(self):
        write_gate.bind(self.ident)
        write_gate.begin_backup()
        other = dict(self.ident, product='Something Else')
        write_gate.bind(other)
        self.assertFalse(write_gate.finish_backup(True, 'x', self.ident))
        write_gate.bind(self.ident)
        self.assertEqual(write_gate.status(), write_gate.LOCKED)

    def test_new_firmware_asks_again(self):
        write_gate.bind(self.ident)
        write_gate.begin_backup()
        write_gate.finish_backup(True, 'x', self.ident)
        write_gate.bind(write_gate.make_identity(0x3537, 0x103D, 'Tarantula 8K PC', 0x0250,
                                                 'Tarantula Pro 8K'))
        self.assertEqual(write_gate.status(), write_gate.LOCKED)

    def test_manual_export_walk_does_not_approve(self):
        write_gate.bind(self.ident)
        write_gate.begin_backup()
        write_gate.end_backup()
        self.assertEqual(write_gate.status(), write_gate.LOCKED)

    def test_revoke(self):
        write_gate.bind(self.ident)
        write_gate.begin_backup()
        write_gate.finish_backup(True, 'x', self.ident)
        self.assertTrue(write_gate.revoke())
        self.assertEqual(write_gate.status(), write_gate.LOCKED)

    def test_not_required_means_approved(self):
        with patch.object(hostos, 'REQUIRES_WRITE_APPROVAL', False):
            self.assertEqual(write_gate.status(), write_gate.APPROVED)
            self.assertTrue(write_gate.allows((0x0F, 0x03)))


class RegisterMapCheck(unittest.TestCase):
    """doctor._plausibility: the read-only test that a pad's active profile
    decodes sensibly with the map Deadband would write it with."""

    def defaults(self, prof):
        vals = {}
        for addr, ln in prof.read_fields():
            vals[addr] = [0] * ln
        for lo, hi in ((prof.LT_DZ_MIN, prof.LT_DZ_MAX), (prof.ST_DZ_MIN, prof.ST_DZ_MAX),
                       (prof.RT_DZ_MIN, prof.RT_DZ_MAX), (prof.RS_DZ_MIN, prof.RS_DZ_MAX)):
            vals[lo] = [0x00, 0x32]                # 5.0%
            vals[hi] = [0x03, 0xE8]                # 100.0%
        vals[prof.POLL_RATE] = [2]                 # 1000 Hz
        return vals

    def test_textbook_defaults_pass(self):
        import doctor
        prof = profiles.TARANTULA_PRO_8K
        rows = doctor._plausibility(prof, self.defaults(prof))
        self.assertTrue(rows)
        self.assertFalse([r for r in rows if r[3] is False], rows)
        self.assertIn('1000 Hz ok', [r[2] for r in rows])

    def test_a_different_layout_is_flagged(self):
        import doctor
        prof = profiles.TARANTULA_PRO_8K
        vals = self.defaults(prof)
        vals[prof.ST_DZ_MIN] = [0xA5, 0x5A]        # 4235.4%: not this map
        vals[prof.POLL_RATE] = [0x41]
        bad = [r for r in doctor._plausibility(prof, vals) if r[3] is False]
        self.assertEqual(len(bad), 3)              # out of range, min > max, poll code

    def test_an_all_zero_region_is_flagged(self):
        import doctor
        prof = profiles.TARANTULA_PRO_8K
        vals = {addr: [0] * ln for addr, ln in prof.read_fields()}
        bad = [r for r in doctor._plausibility(prof, vals) if r[3] is False]
        self.assertTrue(any(r[2] == 'ZERO MAXIMUM' for r in bad), bad)

    def test_missing_reply_is_flagged(self):
        import doctor
        prof = profiles.TARANTULA_PRO_8K
        vals = self.defaults(prof)
        vals[prof.ST_DZ_MAX] = None
        self.assertTrue([r for r in doctor._plausibility(prof, vals) if r[2] == 'NO REPLY'])


class HostOs(unittest.TestCase):
    def test_xdg_data_home_wins(self):
        with patch.dict(os.environ, {'XDG_DATA_HOME': os.path.join('x', 'y')}):
            self.assertEqual(hostos.user_data_dir(), os.path.join('x', 'y'))

    def test_windows_data_dir_is_localappdata(self):
        env = {k: v for k, v in os.environ.items() if k != 'XDG_DATA_HOME'}
        env['LOCALAPPDATA'] = 'C:\\Users\\me\\AppData\\Local'
        with patch.dict(os.environ, env, clear=True), patch.object(hostos, 'IS_WINDOWS', True):
            self.assertEqual(hostos.user_data_dir(), 'C:\\Users\\me\\AppData\\Local')

    def test_windows_11_is_named_from_the_build(self):
        import platform
        with patch.object(hostos, 'IS_WINDOWS', True), patch.object(hostos, 'IS_MACOS', False), \
                patch.object(platform, 'version', return_value='10.0.26100'):
            self.assertEqual(hostos.os_description(), 'Windows 11 (build 26100)')

    def test_no_raw_usb_claiming_on_windows(self):
        if hostos.IS_WINDOWS:
            self.assertFalse(hostos.RAW_USB_HID_FALLBACK)
            self.assertFalse(hostos.RAW_USB_CONFIG)
            self.assertFalse(hostos.HAS_EVDEV)
            self.assertTrue(hostos.PER_COLLECTION_HID_PATHS)


@unittest.skipUnless(sys.platform == 'win32', 'needs the Windows HID API')
class RealWindowsApi(unittest.TestCase):
    def test_caps_of_a_missing_path_is_none(self):
        self.assertIsNone(winhid.caps('\\\\?\\HID#VID_0000&PID_0000#0&0&0&0#' + GUID))

    def test_reader_imports_without_fcntl(self):
        import reader                                    # noqa: F401
        self.assertIsNone(reader.fcntl)

    def test_discovery_runs(self):
        self.assertIsInstance(gs_common.find_controllers(), list)


if __name__ == '__main__':
    unittest.main()
