#!/usr/bin/env python3
"""Package a static ARM64 BusyBox as a diskless rootfs (Linux or macOS, no sudo)."""
import argparse
import gzip
from pathlib import Path
import stat
import struct
import sys

# Built-ins needed by this ARM64 QEMU virt boot recipe, in addition to the camera.
REQUIRED = ('ARM64', 'BLK_DEV_INITRD', 'RD_GZIP', 'BINFMT_ELF', 'BINFMT_SCRIPT',
            'DEVTMPFS', 'PROC_FS', 'SYSFS', 'TMPFS', 'TTY',
            'SERIAL_AMBA_PL011', 'SERIAL_AMBA_PL011_CONSOLE', 'PCI',
            'PCI_HOST_GENERIC')
INIT = b'''#!/bin/sh
export PATH=/bin:/sbin
/bin/busybox --install -s /bin
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t tmpfs -o mode=1777,size=128m tmpfs /tmp
echo
echo "VCAM minimal rootfs ready (RAM only; no SSH or capture utilities)."
echo "Check: dmesg | grep -E 'vcam|xhci'; ls /dev/video*"
echo "Exit QEMU: Ctrl-a then x. Power off guest: poweroff -f"
exec /bin/busybox init
'''


def validate_busybox(path):
    data = path.read_bytes()
    if len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('BusyBox must be a Linux ELF64 little-endian executable, not a Mac binary')
    kind, machine = struct.unpack_from('<HH', data, 16)
    if machine != 183 or kind not in (2, 3):
        raise ValueError('BusyBox must target ARM64 (AArch64) Linux')
    offset = struct.unpack_from('<Q', data, 32)[0]
    size, count = struct.unpack_from('<HH', data, 54)
    if not count or size < 56 or offset + size * count > len(data):
        raise ValueError('BusyBox has an invalid ELF program-header table')
    for index in range(count):
        if struct.unpack_from('<I', data, offset + index * size)[0] == 3:
            raise ValueError('BusyBox must be statically linked (PT_INTERP found); use busybox-static')
    return data


def verify_config(path):
    values = dict(line.split('=', 1) for line in path.read_text().splitlines()
                  if line.startswith('CONFIG_') and '=' in line)
    missing = [f'CONFIG_{key}=y' for key in REQUIRED if values.get('CONFIG_' + key) != 'y']
    if missing:
        raise ValueError('Reconfigure/rebuild the kernel; missing built-ins: ' + ', '.join(missing))


def archive(busybox):
    """Write newc directly so macOS needs neither mknod privileges nor GNU cpio."""
    result = bytearray()
    inode = 0

    def entry(name, mode, data=b'', rdev=(0, 0)):
        nonlocal inode
        inode += 1
        name_bytes = name.encode() + b'\0'
        fields = (inode, mode, 0, 0, 2 if stat.S_ISDIR(mode) else 1, 0,
                  len(data), 0, 0, *rdev, len(name_bytes), 0)
        result.extend(b'070701' + ''.join(f'{n:08x}' for n in fields).encode())
        result.extend(name_bytes)
        result.extend(b'\0' * (-len(result) % 4))
        result.extend(data)
        result.extend(b'\0' * (-len(result) % 4))

    for name in ('bin', 'sbin', 'dev', 'proc', 'sys', 'tmp', 'etc', 'root', 'mnt'):
        entry(name, stat.S_IFDIR | (0o1777 if name == 'tmp' else 0o755))
    entry('dev/console', stat.S_IFCHR | 0o600, rdev=(5, 1))
    entry('dev/null', stat.S_IFCHR | 0o666, rdev=(1, 3))
    entry('bin/busybox', stat.S_IFREG | 0o755, busybox)
    entry('bin/sh', stat.S_IFLNK | 0o777, b'busybox')
    entry('init', stat.S_IFREG | 0o755, INIT)
    entry('etc/inittab', stat.S_IFREG | 0o644,
          b'ttyAMA0::askfirst:-/bin/sh\n::ctrlaltdel:/bin/poweroff -f\n')
    entry('etc/passwd', stat.S_IFREG | 0o644, b'root:x:0:0:root:/root:/bin/sh\n')
    entry('etc/group', stat.S_IFREG | 0o644, b'root:x:0:\n')
    entry('TRAILER!!!', 0)
    result.extend(b'\0' * (-len(result) % 512))
    return gzip.compress(bytes(result), mtime=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--busybox', required=True, type=Path,
                        help='static ARM64 Linux BusyBox with sh, init, mount, mkdir and --install support')
    parser.add_argument('--kernel-config', required=True, type=Path, help='matching kernel build .config')
    parser.add_argument('--output', required=True, type=Path, help='new initramfs.cpio.gz path; never overwritten')
    args = parser.parse_args()
    verify_config(args.kernel_config.expanduser())
    data = archive(validate_busybox(args.busybox.expanduser()))
    output = args.output.expanduser()
    with output.open('xb') as stream:
        stream.write(data)
    print(f'Created {output.resolve()} ({len(data)} bytes)')
    print('Launch with --initrd PATH --append rdinit=/init (no --disk or root= needed).')
    print('Serial shell only; volatile storage, no SSH, Python or v4l2-ctl.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as exc:
        sys.exit(f'error: {exc}')
