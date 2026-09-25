import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

MCAP = sys.argv.pop(1)


class RamTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.rom = self.root / 'fixture.image'
        data = bytearray(0x1eca0)
        data[12:24] = b'IDT MONITOR '
        words = {0: 0x08f00007, 0x2a0: 0x3c020080, 0x2a8: 0xac22c180,
                 0x2ac: 0x3c020080, 0x2b4: 0xac22c1c4,
                 0x1ec88: 0x0cf09709, 0x1ec90: 0x24030005,
                 0x1ec94: 0x14430002, 0x1ec9c: 0x3c040080}
        for offset, value in words.items():
            data[offset:offset+4] = value.to_bytes(4, 'big')
        self.rom.write_bytes(data)

    def run_guest(self, *args):
        return subprocess.run([MCAP, '--temporary', '--fresh', '--device', 'datarover840', '--rom', str(self.rom), '--headless', '-n', '0', *args],
                              capture_output=True, text=True, timeout=10)

    def test_rom_detection_without_mutation(self):
        original = self.rom.read_bytes()
        result = self.run_guest()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('RAM: 8388608 bytes (ROM size constants)', result.stderr)
        self.assertEqual(self.rom.read_bytes(), original)

    def test_explicit_override(self):
        result = self.run_guest('--ram', '16')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('RAM: 16777216 bytes (--ram)', result.stderr)

    def test_snapshot_size_takes_precedence_and_explicit_conflict_fails(self):
        state = self.root / 'large.state'
        self.assertEqual(self.run_guest('--ram', '16', '--save-state', str(state)).returncode, 0)
        original = state.read_bytes()
        result = self.run_guest('--load-state', str(state))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('RAM: 16777216 bytes (saved state)', result.stderr)
        self.assertNotEqual(self.run_guest('--ram', '8', '--load-state', str(state)).returncode, 0)
        self.assertEqual(state.read_bytes(), original)

    def test_unknown_or_inconsistent_rom_falls_back(self):
        data = bytearray(self.rom.read_bytes())
        data[0x1ec9c:0x1eca0] = bytes.fromhex('3c040040')
        self.rom.write_bytes(data)
        result = self.run_guest()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('RAM: 4194304 bytes (unrecognized ROM layout; default)', result.stderr)

    def test_invalid_sizes(self):
        for value in ['0', '-1', '61', '4096', '4294967304', '8oops', '999999999999999999999999']:
            with self.subTest(value=value):
                self.assertNotEqual(self.run_guest('--ram', value).returncode, 0)

    def test_bad_state_is_rejected(self):
        state = self.root / 'bad.state'
        state.write_bytes(b'not a snapshot')
        self.assertNotEqual(self.run_guest('--load-state', str(state)).returncode, 0)

    def test_a_devices_unreadable_state_starts_it_from_the_rom(self):
        """The same file, found rather than named, boots the ROM instead.

        Refusing took the whole session down: the window is opened above this
        and closes when it returns, so choosing a device whose state had gone
        stale made the emulator vanish rather than start the machine that was
        asked for. Named on the command line it is still fatal, which is what
        test_bad_state_is_rejected above holds.
        """
        state = self.rom.with_suffix('.state')
        state.write_bytes(b'not a snapshot')
        result = subprocess.run(
            [MCAP, '--device', 'datarover840', '--rom', str(self.rom),
             '--headless', '-n', '0'],
            capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('from its ROM instead', result.stderr)
        # Built at its own default size, not at one read out of that file.
        self.assertIn('RAM: 8388608 bytes (ROM size constants)', result.stderr)


if __name__ == '__main__':
    unittest.main()
