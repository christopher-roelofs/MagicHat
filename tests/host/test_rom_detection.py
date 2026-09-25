"""Content detection and launcher routing; real ROM checks skip if absent."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

EXE = sys.argv.pop(1)
ROOT = pathlib.Path(__file__).resolve().parents[2]


class DetectionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.rom = pathlib.Path(self.tmp.name) / 'arbitrary.bin'

    def run_rom(self, *args):
        return subprocess.run([EXE, '--temporary', '--fresh', '--rom', str(self.rom), '--headless',
                               '-n', '0', *args], capture_output=True,
                              text=True, timeout=15)

    def source(self, name):
        p = ROOT / 'roms' / name
        if not p.is_file():
            self.skipTest(f'local ROM absent: {name}')
        return p.read_bytes()

    def test_unknown_and_misleading_filename(self):
        self.rom = self.rom.with_name('PIC-2000.rom')
        self.rom.write_bytes(bytes(4 * 1024 * 1024))
        result = self.run_rom()
        self.assertEqual(result.returncode, 2)
        self.assertIn('unknown or ambiguous', result.stderr)

    def test_empty_and_truncated(self):
        for data in (b'', b'IDT MONITOR ', b'\x00\x10\x00\x00\x0e\x00\x02\x1e'):
            self.rom.write_bytes(data)
            self.assertNotEqual(self.run_rom().returncode, 0)

    def test_explicit_experimental_machine(self):
        self.rom.write_bytes(bytes(4096))
        r = self.run_rom('--device', 'datarover840')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('selected explicitly', r.stderr)

    def test_mips_renamed_and_modified(self):
        for name in ('Data Rover 840/DataRover-8040-USA.image',
                     'Data Rover 840/DataRover-8040-Japan.image',
                     'Rosemary SDK/MagicCap-USA.image'):
            with self.subTest(name=name):
                data = bytearray(self.source(name))
                data[-1] ^= 0x55
                self.rom.write_bytes(data)
                r = self.run_rom()
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn('DataRover 840 (MIPS)', r.stderr)
                self.assertEqual(self.rom.read_bytes(), data)

    def test_patched_ram_still_detects_device(self):
        data = bytearray(self.source('Data Rover 840/DataRover-8040-USA.image'))
        for offset,word in ((0x2a0,0x3c020080),(0x2ac,0x3c020080),(0x1ec9c,0x3c040080)):
            data[offset:offset+4] = word.to_bytes(4,'big')
        self.rom.write_bytes(data)
        r = self.run_rom()
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn('DataRover 840 (MIPS)',r.stderr)
        self.assertIn('RAM: 8388608 bytes (ROM size constants)',r.stderr)

    def test_68k_devices_renamed_and_modified(self):
        for name,device,code in (
            ('Sony PIC 2000/PIC-2000.rom','Sony PIC-2000',0),
            ('Sony PIC 1000/PIC-1000.rom','Sony PIC-1000',2),
            ('Sony HIX 300/hix300-mc19-c2.rom','Sony HIX-300',0),
            ('Motorola Envoy/envoy-1.0.rom','Motorola Envoy',0),
            ('Motorola Envoy/envoy-1.0-pt4.rom','Motorola Envoy',0),
            ('Motorola Envoy/envoy-1.0-mc31-b10.rom','Motorola Envoy',0),
        ):
            with self.subTest(device=device):
                data = bytearray(self.source(name))
                data[-1] ^= 0x55
                self.rom.write_bytes(data)
                r = self.run_rom()
                self.assertEqual(r.returncode, code, r.stderr)
                self.assertIn(device+' (68k)', r.stderr)
                if code: self.assertIn('hardware is not implemented', r.stderr)

    def test_envoy_explicit_and_saved_identity(self):
        self.rom.write_bytes(self.source('Motorola Envoy/envoy-1.0.rom'))
        r = self.run_rom('--device', 'envoy')
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_rom('--device', 'pic2000')
        self.assertEqual(r.returncode, 2)
        self.assertIn('conflicts', r.stderr)
        # A device puts its state beside its firmware, and identifies itself
        # from the image rather than from anything the state carries.
        r = subprocess.run([EXE, '--rom', str(self.rom), '--fresh',
                            '--headless', '-n', '0'], capture_output=True,
                           text=True, timeout=15)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('Motorola Envoy (68k)', r.stderr)
        self.assertTrue(self.rom.with_suffix('.state').exists())

    def test_bad_reset_and_conflicting_identity(self):
        original = self.source('Sony PIC 2000/PIC-2000.rom')
        for mutation in ('reset','identity'):
            data = bytearray(original)
            if mutation == 'reset': data[4:8] = bytes(4)
            else: data[-32:-24] = b',MOTO,1,'
            self.rom.write_bytes(data)
            self.assertIn('unknown or ambiguous', self.run_rom().stderr)

    def test_model_text_alone_is_not_detection(self):
        data = bytearray(4*1024*1024)
        text = b'PIC-2000 ,SONY,2, IDT MONITOR Apollo'
        data[100:100+len(text)] = text
        self.rom.write_bytes(data)
        self.assertIn('unknown or ambiguous',self.run_rom().stderr)

    def test_device_conflict(self):
        self.rom.write_bytes(self.source('Sony PIC 2000/PIC-2000.rom'))
        r = self.run_rom('--device','datarover840')
        self.assertEqual(r.returncode,2)
        self.assertIn('conflicts',r.stderr)

    def test_input_value_is_not_launcher_option(self):
        self.rom.write_bytes(self.source('Data Rover 840/DataRover-8040-USA.image'))
        for value in ('--rom','--device','--help'):
            r = self.run_rom('--input',value)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertIn('ROM identification: DataRover',r.stderr)


if __name__ == '__main__':
    unittest.main()
