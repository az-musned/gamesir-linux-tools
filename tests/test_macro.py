import unittest

import controller_profile as profiles
import vendors.gamesir.macro as macro


class MacroBlockTests(unittest.TestCase):
    def test_short_macro_clears_the_old_tail(self):
        # the firmware plays past `count` until an empty step (#51)
        out = macro.block_bytes([{'target': 9, 'hold': 100, 'delay': 100}], 31)
        self.assertEqual(out[:6], [1, 9, 0, 100, 0, 100])
        self.assertEqual(len(out), 1 + 31 * macro.EVENT_LEN)
        self.assertFalse(any(out[6:]))

    def test_macro_writes_stay_inside_their_paddle_block(self):
        # The block starts 2 bytes BEFORE the base (the button's turbo on/rate),
        # so the next block's turbo bytes sit at stride-2 and stride-1.
        for prof in profiles.ALL:
            if not prof.has_macros:
                continue
            bases = sorted(b for _n, b in prof.MACRO_SLOTS)
            stride = min(b - a for a, b in zip(bases, bases[1:]))
            full = [{'target': 9, 'hold': 1, 'delay': 1}] * prof.macro_max
            region = macro.region_for(prof)
            for evs in ([], full):
                end = macro.COUNT_OFF + len(macro.block_bytes(evs, prof.macro_max, region))
                self.assertLessEqual(end, stride - 2, prof.name)
            self.assertEqual(len(macro.slot_sizes(region, prof.macro_max)), prof.macro_max,
                             prof.name)

    def test_8k_keeps_the_setting_byte_at_the_end_of_each_block(self):
        end = macro.COUNT_OFF + len(macro.block_bytes([], profiles.G7_8K.macro_max))
        self.assertLessEqual(end, 0xa8)


class MacroFlagsAndGeometryTests(unittest.TestCase):
    """GameSir Connect's codec (pedrosegato/gamesir-reverse-engineering, verified
    on a Cyclone 2): flags bit 0 enabled, bit 1 hold, bit 2 loop; BE16 loop
    interval; the block's last step slot is [target, hold] only."""

    def block(self, prof, flags, loop_ms, steps):
        base = dict(prof.MACRO_SLOTS)['L4']
        region = macro.region_for(prof)
        body = macro.block_bytes(steps, prof.macro_max, region)
        mem = {base + macro.FLAGS_OFF: macro.head_bytes(flags, loop_ms),
               base + macro.COUNT_OFF: [body[0]]}
        raw = body[1:]
        for addr, ln in macro.read_addrs(base, prof.macro_max, region)[2:]:
            off = addr - (base + macro.EVENTS_OFF)
            mem[addr] = raw[off:off + ln]
        return mem, base, region

    def test_decodes_connect_flags_and_interval(self):
        prof = profiles.TARANTULA_PRO_8K
        mem, base, region = self.block(prof, 0x07, 250, [{'target': 9, 'hold': 50, 'delay': 80}])
        m = macro.decode(mem, base, prof.macro_max, region)
        self.assertEqual((m['enable'], m['hold'], m['loop'], m['loop_ms']), (True, True, True, 250))
        self.assertEqual(m['events'], [{'target': 9, 'hold': 50, 'delay': 80}])

    def test_toggling_enable_keeps_mode_and_loop(self):
        self.assertEqual(macro.flags_byte(0x07, enable=False), 0x06)
        self.assertEqual(macro.flags_byte(0x06, enable=True), 0x07)
        self.assertEqual(macro.flags_byte(0x05, hold=True), 0x07)
        self.assertEqual(macro.flags_byte(0x07, loop=False), 0x03)
        self.assertEqual(macro.flags_byte(0x81, loop=True), 0x85)     # unknown bit kept

    def test_head_is_big_endian(self):
        self.assertEqual(macro.head_bytes(0x05, 0x1234), [0x05, 0x12, 0x34])
        self.assertEqual(macro.head_bytes(0x01, 99999), [0x01, 0xff, 0xff])   # clamped

    def test_full_macro_last_step_has_no_delay(self):
        for prof in (profiles.CYCLONE, profiles.TARANTULA_PRO_8K):
            steps = [{'target': 9, 'hold': 7, 'delay': 9}] * prof.macro_max
            mem, base, region = self.block(prof, 0x01, 0, steps)
            evs = macro.decode(mem, base, prof.macro_max, region)['events']
            self.assertEqual(len(evs), prof.macro_max, prof.name)
            self.assertEqual(evs[-1], {'target': 9, 'hold': 7, 'delay': 0}, prof.name)
            self.assertEqual(evs[-2]['delay'], 9, prof.name)

    def test_block_ends_exactly_at_the_block_end(self):
        # Cyclone: 159-byte blocks -> the step region ends at base+0x9c
        r = macro.region_for(profiles.CYCLONE)
        self.assertEqual(macro.COUNT_OFF + len(macro.block_bytes([], 30, r)) - 1, 0x9c)
        # 8K / Tarantula: 169-byte blocks -> base+0xa6; +0xa7/+0xa8 are the next
        # block's turbo on / rate (the 8K's "0x0a setting byte", #51)
        r = macro.region_for(profiles.TARANTULA_PRO_8K)
        self.assertEqual(macro.COUNT_OFF + len(macro.block_bytes([], 32, r)) - 1, 0xa6)

    def test_one_byte_flags_from_an_older_backup(self):
        prof = profiles.CYCLONE
        mem, base, region = self.block(prof, 0x03, 0, [])
        mem[base + macro.FLAGS_OFF] = [0x03]
        m = macro.decode(mem, base, prof.macro_max, region)
        self.assertEqual((m['enable'], m['hold'], m['loop_ms']), (True, True, 0))


if __name__ == '__main__':
    unittest.main()
