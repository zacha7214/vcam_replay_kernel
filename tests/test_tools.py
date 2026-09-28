import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


images = module('images', 'tools/vcam_images.py')
setup = module('setup', 'scripts/setup-kernel.py')


class ImageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)

    def test_pgm_comments_crlf_and_whitespace_first_pixel(self):
        pixels = b'\n #\r' + bytes(range(252))
        path = self.path / 'test.pgm'
        path.write_bytes(b'P5\r\n# camera\n16 16\n255\r\n' + pixels)
        self.assertEqual(images.read_pgm(path), (16, 16, pixels))

    def test_reject_truncated_and_16bit_pgm(self):
        path = self.path / 'test.pgm'
        for data in (b'P5\n16 16\n255\nshort', b'P5\n16 16\n65535\n', b'P5\n# no end'):
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                images.read_pgm(path)

    def test_capture_verification_corruption_truncation_and_order(self):
        output = self.path / 'frames.raw'
        a, b = bytes(range(256)), bytes(reversed(range(256)))
        with contextlib.redirect_stdout(io.StringIO()):
            images.write_frames(output, 16, 16, 'grey', [a, b])
            manifest = output.with_suffix('.raw.json')
            capture = self.path / 'capture.raw'
            capture.write_bytes(a + b + a)
            self.assertEqual(images.verify(manifest, capture, True), 3)
            capture.write_bytes(b + a)
            self.assertEqual(images.verify(manifest, capture), 2)
            with self.assertRaises(ValueError):
                images.verify(manifest, capture, True)
            for bad in (b'', a[:-1], a + bytes(256)):
                capture.write_bytes(bad)
                with self.assertRaises(ValueError):
                    images.verify(manifest, capture)

    def test_geometry(self):
        for dims in ((0, 16, 'grey'), (8193, 16, 'grey'), (17, 16, 'yuyv')):
            with self.assertRaises(ValueError):
                images.geometry(*dims)
        self.assertEqual(images.geometry(16, 16, 'yuyv'), 512)

    def test_pack_cli_and_no_overwrite(self):
        directory = self.path / 'input'
        directory.mkdir()
        for name, byte in [('b', 2), ('a', 1)]:
            (directory / f'{name}.pgm').write_bytes(b'P5\n16 16\n255\n' + bytes([byte]) * 256)
        output = self.path / 'out.raw'
        cmd = ['python3', str(ROOT / 'tools/vcam_images.py'), 'pack', str(directory), str(output)]
        result = subprocess.run(cmd, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_bytes(), bytes([1]) * 256 + bytes([2]) * 256)
        self.assertNotEqual(subprocess.run(cmd, capture_output=True).returncode, 0)


class SetupTests(unittest.TestCase):
    """Installer filesystem/error tests; fake make simulates Kconfig resolution.

    Real olddefconfig and a full vmlinux link are separate integration checks.
    """
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='vcam test ')
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.kernel = self.path / 'linux source'
        for name in ('Makefile', 'Kconfig', 'drivers/media/Kconfig', 'drivers/media/Makefile',
                     'include/media/videobuf2-v4l2.h'):
            path = self.kernel / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('# original\n')
        self.output = self.path / 'build output'
        self.output.mkdir()
        (self.output / '.config').write_text('CONFIG_UNRELATED=y\n')
        script = self.kernel / 'scripts/config'
        script.parent.mkdir()
        script.write_text('''#!/usr/bin/env python3
import sys
from pathlib import Path
p = Path(sys.argv[2])
with p.open('a') as f:
    for symbol in sys.argv[4::2]:
        f.write('CONFIG_' + symbol + '=y\\n')
''')
        script.chmod(0o755)
        make = self.path / 'fake-make'
        make.write_text('''#!/usr/bin/env python3
import os, sys
from pathlib import Path
p = Path(next(x[2:] for x in sys.argv if x.startswith('O='))) / '.config'
with p.open('a') as f:
    for symbol in %r:
        f.write('CONFIG_' + symbol + '=y\\n')
if os.environ.get('DROP_VIDEO'):
    p.write_text(p.read_text().replace('CONFIG_VIDEO_DEV=y', '# CONFIG_VIDEO_DEV is not set'))
''' % (setup.REQUIRED[len(setup.REQUESTED):],))
        make.chmod(0o755)
        self.env = dict(os.environ, MAKE=str(make))
        self.env.pop('KCONFIG_CONFIG', None)
        self.command = ['python3', str(ROOT / 'scripts/setup-kernel.py'), str(self.kernel),
                        '--output', str(self.output), '--arch', 'arm64']

    def run_setup(self, *args):
        return subprocess.run(self.command + list(args), env=self.env, capture_output=True, text=True)

    def test_install_repeat_check_and_backup(self):
        for _ in range(2):
            result = self.run_setup()
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_setup('--check').returncode, 0)
        self.assertFalse((self.kernel / 'drivers/media/vcam-replay/vcam_replay.mod.c').exists())
        self.assertEqual((self.kernel / 'drivers/media/Kconfig').read_text().count(
            'source "drivers/media/vcam-replay/Kconfig"'), 1)
        self.assertIn('CONFIG_UNRELATED=y', (self.output / '.config').read_text())
        backups = sorted((self.kernel / '.vcam-backups').iterdir())
        self.assertEqual((backups[0] / 'config').read_text(), 'CONFIG_UNRELATED=y\n')
        installed = self.kernel / 'drivers/media/vcam-replay/vcam_core.c'
        installed.write_text('stale')
        self.assertNotEqual(self.run_setup('--check').returncode, 0)

    def test_optional_rootfs_is_packaged_and_not_overwritten(self):
        from test_rootfs import elf, rootfs
        busybox = self.path / 'busybox'
        busybox.write_bytes(elf())
        result = self.run_setup('--rootfs-busybox', str(busybox))
        self.assertEqual(result.returncode, 0, result.stderr)
        rootfs.verify_config(self.output / '.config')
        archive = self.output / 'initramfs.cpio.gz'
        self.assertTrue(archive.is_file())
        before = (self.output / '.config').read_bytes()
        result = self.run_setup('--rootfs-busybox', str(busybox))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.output / '.config').read_bytes(), before)

    def test_unresolved_dependency_fails(self):
        self.env['DROP_VIDEO'] = '1'
        result = self.run_setup()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('CONFIG_VIDEO_DEV=unset', result.stderr)

    def test_missing_config_does_not_mutate_source(self):
        (self.output / '.config').unlink()
        result = self.run_setup()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.kernel / '.vcam-backups').exists())


class CaptureTests(unittest.TestCase):
    def test_capture_and_verify_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            raw = path / 'source.raw'
            with contextlib.redirect_stdout(io.StringIO()):
                images.write_frames(raw, 16, 16, 'grey', [bytes(range(256))])
            ctl = path / 'v4l2-ctl'
            ctl.write_text('#!/usr/bin/env python3\nimport sys\nsys.stdout.buffer.write(bytes(range(256)) * 3)\n')
            ctl.chmod(0o755)
            command = ['python3', str(ROOT / 'scripts/capture-guest.py'),
                       str(path / 'captured.raw'), '--device', '/dev/mock', '--count', '3',
                       '--manifest', str(raw.with_suffix('.raw.json'))]
            env = dict(os.environ, PATH=str(path) + os.pathsep + os.environ['PATH'])
            result = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('PASS: 3 captured frames', result.stdout)
            self.assertNotEqual(subprocess.run(command, env=env, capture_output=True).returncode, 0)

    def test_stalled_capture_times_out(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            ctl = path / 'v4l2-ctl'
            ctl.write_text('#!/usr/bin/env python3\nimport time\ntime.sleep(5)\n')
            ctl.chmod(0o755)
            command = ['python3', str(ROOT / 'scripts/capture-guest.py'),
                       str(path / 'captured.raw'), '--device', '/dev/mock', '--timeout', '0.1']
            env = dict(os.environ, PATH=str(path) + os.pathsep + os.environ['PATH'])
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('timed out', result.stderr)


if __name__ == '__main__':
    unittest.main()
