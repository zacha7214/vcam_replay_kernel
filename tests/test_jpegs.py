import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import vcam_jpegs
try:
    from PIL import Image
except ImportError:
    Image = None


@unittest.skipIf(Image is None, 'JPEG packing requires optional Pillow dependency')
class JPEGTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.inputs = self.path / 'images'
        self.inputs.mkdir()
        self.output = self.path / 'frames.raw'

    def test_order_padding_and_verification(self):
        Image.new('RGB', (32, 16), 'white').save(self.inputs / '02.JPG')
        Image.new('RGB', (16, 16), 'black').save(self.inputs / '01.jpeg')
        self.assertEqual(vcam_jpegs.pack(self.inputs, self.output, 16, 16), 2)
        data = self.output.read_bytes()
        self.assertEqual(len(data), 512)
        self.assertEqual(data[:256], bytes(256))
        self.assertEqual(data[256:320], bytes(64))
        self.assertEqual(data[320:448], bytes([255]) * 128)
        manifest = json.loads(self.output.with_suffix('.raw.json').read_text())
        self.assertEqual(manifest['sources'], ['01.jpeg', '02.JPG'])
        result = subprocess.run([sys.executable, str(ROOT / 'tools/vcam_images.py'), 'verify',
                                 str(self.output) + '.json', str(self.output), '--strict-sequence'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_exif_orientation(self):
        source = Image.new('RGB', (32, 16), 'white')
        exif = source.getexif()
        exif[274] = 6
        source.save(self.inputs / 'rotated.jpg', exif=exif)
        vcam_jpegs.pack(self.inputs, self.output, 16, 32)
        self.assertEqual(self.output.read_bytes(), bytes([255]) * 512)

    def test_corrupt_input_removes_partial_output(self):
        Image.new('RGB', (16, 16)).save(self.inputs / '01.jpg')
        (self.inputs / '02.jpg').write_bytes(b'corrupt')
        with self.assertRaises(ValueError):
            vcam_jpegs.pack(self.inputs, self.output)
        self.assertFalse(self.output.exists())
        self.assertFalse(Path(str(self.output) + '.json').exists())

    def test_refuses_overwrite_and_empty_input(self):
        with self.assertRaises(ValueError):
            vcam_jpegs.pack(self.inputs, self.output)
        Image.new('RGB', (16, 16)).save(self.inputs / '01.jpg')
        manifest = Path(str(self.output) + '.json')
        manifest.write_text('keep')
        with self.assertRaises(ValueError):
            vcam_jpegs.pack(self.inputs, self.output)
        self.assertEqual(manifest.read_text(), 'keep')
        self.assertFalse(self.output.exists())
        manifest.unlink()
        self.output.write_bytes(b'keep')
        with self.assertRaises(ValueError):
            vcam_jpegs.pack(self.inputs, self.output)
        self.assertEqual(self.output.read_bytes(), b'keep')
