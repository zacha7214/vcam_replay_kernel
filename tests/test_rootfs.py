"""Validate portable rootfs packaging and reject unbootable executable inputs."""
import gzip
import importlib.util
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('rootfs', ROOT / 'scripts/build-rootfs.py')
rootfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rootfs)


def elf(machine=183, segment=1):
    data = bytearray(120)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HH', data, 16, 2, machine)
    struct.pack_into('<Q', data, 32, 64)
    struct.pack_into('<HH', data, 54, 56, 1)
    struct.pack_into('<I', data, 64, segment)
    return bytes(data)


class RootfsTests(unittest.TestCase):
    def test_reject_wrong_arch_dynamic_and_truncated_elf(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'busybox'
            for data in (b'not ELF', elf(62), elf(segment=3), elf()[:90]):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    rootfs.validate_busybox(path)
            path.write_bytes(elf())
            self.assertEqual(rootfs.validate_busybox(path), elf())

    def test_newc_structure_permissions_devices_and_contents(self):
        data = gzip.decompress(rootfs.archive(elf()))
        entries = {}
        offset = 0
        while True:
            self.assertEqual(data[offset:offset + 6], b'070701')
            fields = [int(data[offset + 6 + i * 8:offset + 14 + i * 8], 16) for i in range(13)]
            offset += 110
            name = data[offset:offset + fields[11] - 1].decode()
            offset = (offset + fields[11] + 3) & ~3
            contents = data[offset:offset + fields[6]]
            offset = (offset + fields[6] + 3) & ~3
            if name == 'TRAILER!!!':
                break
            parent = str(Path(name).parent)
            if parent != '.':
                self.assertIn(parent, entries)
            entries[name] = (fields, contents)
        self.assertEqual(entries['bin/busybox'][1], elf())
        self.assertEqual(entries['bin/sh'][1], b'busybox')
        self.assertEqual(entries['init'][0][1], stat.S_IFREG | 0o755)
        self.assertEqual(entries['dev/console'][0][9:11], [5, 1])
        self.assertEqual(entries['dev/console'][0][1], stat.S_IFCHR | 0o600)
        self.assertIn(b'ttyAMA0', entries['etc/inittab'][1])

    def test_cli_config_validation_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            binary, config, output = (path / name for name in ('busybox', '.config', 'initramfs.gz'))
            binary.write_bytes(elf())
            config.write_text('')
            command = [sys.executable, str(ROOT / 'scripts/build-rootfs.py'), '--busybox', str(binary),
                       '--kernel-config', str(config), '--output', str(output)]
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertFalse(output.exists())
            config.write_text(''.join(f'CONFIG_{key}=y\n' for key in rootfs.REQUIRED))
            result = subprocess.run(command, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            original = output.read_bytes()
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), original)
