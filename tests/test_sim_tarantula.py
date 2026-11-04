"""End to end over the Windows-shaped HID path: discovery of the vendor
collection, the reader, the write gate, the pre-approval backup walk and a
verified write, against tools/sim_tarantula.py. Run in a subprocess because
the reader thread it starts never returns and owns the shared state."""
import os
import subprocess
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class SimulatedTarantula(unittest.TestCase):
    def test_selftest(self):
        env = dict(os.environ)
        env.pop('DEADBAND_WRITE_APPROVAL', None)      # the self-test turns the gate on
        out = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'sim_tarantula.py'),
                              '--selftest'], capture_output=True, text=True, timeout=240,
                             cwd=ROOT, env=env)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn('RESULT: PASS', out.stdout)


if __name__ == '__main__':
    unittest.main()
