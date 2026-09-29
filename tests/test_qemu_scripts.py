"""Test setup and command generation without building or starting QEMU."""
import importlib.util
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'scripts'
spec = importlib.util.spec_from_file_location('setup_qemu', SCRIPTS / 'setup-qemu.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class QemuScriptTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='qemu tests ')
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.source = self.path / 'qemu'
        for name in ('configure', 'hw/usb/Kconfig', 'hw/usb/meson.build', 'include/hw/usb/usb.h'):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('# existing\n')
        self.kernel = self.path / 'Image'
        self.initrd = self.path / 'initramfs.cpio'
        self.kernel.touch()
        self.initrd.touch()

    def run_script(self, name, *args):
        return subprocess.run([sys.executable, str(SCRIPTS / name), *map(str, args)],
                              text=True, capture_output=True)

    def install(self):
        result = self.run_script('setup-qemu.py', self.source)
        self.assertEqual(result.returncode, 0, result.stderr)

    def launch(self, *args):
        return self.run_script('run-qemu.py', '--kernel', self.kernel,
                               '--initrd', self.initrd, '--dry-run', *args)

    def test_setup_idempotent_backed_up_and_checked(self):
        self.install()
        before = {p: p.read_bytes() for p in setup.plan(self.source)}
        self.install()
        self.assertEqual(before, {p: p.read_bytes() for p in before})
        self.assertEqual(len(list((self.source / '.vcam-backups').iterdir())), 1)
        self.assertEqual(self.run_script('setup-qemu.py', self.source, '--check').returncode, 0)
        (self.source / 'hw/usb/dev-vcam.c').write_text('stale')
        self.assertNotEqual(self.run_script('setup-qemu.py', self.source, '--check').returncode, 0)

    def test_conflicting_hook_leaves_source_unchanged(self):
        path = self.source / 'hw/usb/Kconfig'
        original = 'config USB_VCAM\n    bool\n    default n\n'
        path.write_text(original)
        result = self.run_script('setup-qemu.py', self.source)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(path.read_text(), original)
        self.assertFalse((self.source / 'hw/usb/dev-vcam.c').exists())
        self.assertFalse((self.source / '.vcam-backups').exists())

    def test_build_dry_run_does_not_build(self):
        self.install()
        result = self.run_script('build-qemu.py', '--source', self.source, '--dry-run')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--target-list=aarch64-softmmu', result.stdout)
        self.assertIn('--enable-slirp', result.stdout)
        self.assertFalse((self.source / 'build-vcam').exists())

    def test_local_paths_and_tcg_command(self):
        frames = self.path / 'pixels, with spaces.raw'
        frames.write_bytes(bytes(256))
        result = self.launch('--frames', frames, '--width', 16, '--height', 16,
                             '--ssh-port', 2222)
        self.assertEqual(result.returncode, 0, result.stderr)
        command = shlex.split(result.stdout)
        self.assertEqual(command[command.index('-accel') + 1], 'tcg')
        self.assertEqual(command[command.index('-kernel') + 1], str(self.kernel.resolve()))
        self.assertIn('vcam_replay.devices=0', command[command.index('-append') + 1])
        self.assertTrue(any('pixels,, with spaces.raw' in arg for arg in command))
        self.assertIn('user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22', command)

    def test_disk_snapshot_and_explicit_root(self):
        disk = self.path / 'root.raw'
        disk.touch()
        self.assertNotEqual(self.launch('--disk', disk).returncode, 0)
        result = self.launch('--disk', disk, '--disk-format', 'raw', '--append', 'root=/dev/vda rw')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('-snapshot', shlex.split(result.stdout))
        result = self.launch('--disk', disk, '--append', 'root=/dev/vda rw', '--writable-disk')
        self.assertNotIn('-snapshot', shlex.split(result.stdout))

    def test_reject_bad_frames_and_missing_guest_userspace(self):
        frames = self.path / 'bad.raw'
        frames.write_bytes(b'bad')
        self.assertNotEqual(self.launch('--frames', frames).returncode, 0)
        self.assertNotEqual(self.launch('--format', 'yuyv', '--width', 17).returncode, 0)
        result = self.run_script('run-qemu.py', '--kernel', self.kernel, '--dry-run')
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
