import hashlib
import json
from pathlib import Path
import runpy
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
apply = runpy.run_path(str(ROOT / 'scripts/patch-rom'))['apply']


class PatchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.source, self.manifest, self.output = [root / n for n in ('source', 'patch', 'output')]
        self.source.write_bytes(bytes(range(32)))
        self.spec = {'name': 'test', 'source_sha256': hashlib.sha256(self.source.read_bytes()).hexdigest(),
                     'patches': [{'offset': '0x4', 'before': '04050607', 'after': 'aabbccdd'}]}

    def perform(self):
        self.manifest.write_text(json.dumps(self.spec))
        apply(self.source, self.manifest, self.output)

    def test_separate_file_and_only_requested_bytes(self):
        self.perform()
        self.assertEqual(self.source.read_bytes(), bytes(range(32)))
        self.assertEqual(self.output.read_bytes(), bytes(range(4)) + bytes.fromhex('aabbccdd') + bytes(range(8,32)))

    def test_wrong_image_creates_no_output(self):
        self.source.write_bytes(b'wrong ROM')
        with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
            self.perform()
        self.assertFalse(self.output.exists())

    def test_invalid_patch_creates_no_output(self):
        for patch in [dict(offset='0x4', before='ffffffff', after='aabbccdd'),
                      dict(offset='0x40', before='04', after='aa'),
                      dict(offset='0x4', before='0405', after='aa')]:
            with self.subTest(patch=patch):
                self.spec['patches'] = [patch]
                with self.assertRaises(ValueError):
                    self.perform()
                self.assertFalse(self.output.exists())

    def test_refuses_to_overwrite_input_or_existing_output(self):
        self.output = self.source
        with self.assertRaises(FileExistsError):
            self.perform()
        self.assertEqual(self.source.read_bytes(), bytes(range(32)))

    def test_overlap_is_rejected(self):
        self.spec['patches'] *= 2
        with self.assertRaisesRegex(ValueError, 'overlapping'):
            self.perform()
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
