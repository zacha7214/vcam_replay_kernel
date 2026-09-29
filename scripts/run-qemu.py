#!/usr/bin/env python3
"""Run an ARM64 guest and usb-vcam using local paths on Linux or macOS."""
import argparse
import os
from pathlib import Path
import platform
import shlex
import sys


def local_file(value):
    path = Path(value).expanduser().resolve()
    if not path.is_file():
        raise argparse.ArgumentTypeError(f'file does not exist: {path}')
    return path


def qemu_path(path):
    # QEMU's comma-separated key/value options escape commas by doubling them.
    return str(path).replace(',', ',,')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path,
                        default=Path.home() / 'qemu-11.1.1/build-vcam/qemu-system-aarch64')
    parser.add_argument('--kernel', required=True, type=local_file, help='built ARM64 arch/arm64/boot/Image')
    parser.add_argument('--disk', type=local_file, help='existing bootable Linux root filesystem disk')
    parser.add_argument('--disk-format', choices=['raw', 'qcow2'], default='qcow2')
    parser.add_argument('--initrd', type=local_file, help='optional initramfs (required if no disk)')
    parser.add_argument('--append', default='', help='guest boot arguments, e.g. "root=/dev/vda2 rw"')
    parser.add_argument('--frames', type=local_file, help='packed pixels on this machine; omit for pattern')
    parser.add_argument('--width', type=int, default=640)
    parser.add_argument('--height', type=int, default=480)
    parser.add_argument('--format', choices=['grey', 'yuyv'], default='grey')
    parser.add_argument('--fps', type=int, default=30)
    parser.add_argument('--accel', choices=['tcg', 'kvm', 'hvf'], default='tcg')
    parser.add_argument('--memory', default='2G')
    parser.add_argument('--cpus', type=int, default=2)
    parser.add_argument('--ssh-port', type=int, help='optional localhost port forwarded to guest ssh port 22')
    parser.add_argument('--writable-disk', action='store_true', help='persist writes; default uses QEMU snapshot mode')
    parser.add_argument('--dry-run', action='store_true', help='print the command without requiring a built QEMU')
    args = parser.parse_args()
    if not args.disk and not args.initrd:
        parser.error('supply --disk and/or --initrd containing your Linux guest userspace')
    if args.disk and not args.append.strip():
        parser.error('--disk requires --append with your root= boot arguments; partition layout is not guessed')
    if not (16 <= args.width <= 8192 and 16 <= args.height <= 8192):
        parser.error('dimensions must be 16..8192')
    if args.format == 'yuyv' and args.width % 2:
        parser.error('YUYV requires even width')
    if not (1 <= args.fps <= 100000) or args.cpus < 1:
        parser.error('fps must be 1..100000 and cpus must be positive')
    if args.ssh_port is not None and not 1 <= args.ssh_port <= 65535:
        parser.error('ssh-port must be 1..65535')
    if args.accel == 'kvm' and (platform.system() != 'Linux' or platform.machine() not in ('arm64', 'aarch64')):
        parser.error('ARM64 KVM requires an ARM64 Linux host; use --accel tcg')
    if args.accel == 'hvf' and (platform.system() != 'Darwin' or platform.machine() != 'arm64'):
        parser.error('ARM64 HVF requires Apple silicon macOS; use --accel tcg')
    if args.accel == 'kvm' and not args.dry_run and not os.access('/dev/kvm', os.R_OK | os.W_OK):
        parser.error('/dev/kvm is absent or inaccessible; use --accel tcg')
    qemu = args.qemu.expanduser().resolve()
    if not args.dry_run and not os.access(qemu, os.X_OK):
        parser.error(f'QEMU is not built/executable: {qemu}; run scripts/build-qemu.py first')
    camera = f'usb-vcam,bus=xhci.0,width={args.width},height={args.height},fps={args.fps},format={args.format}'
    if args.frames:
        size = args.width * args.height * (2 if args.format == 'yuyv' else 1)
        length = args.frames.stat().st_size
        if not length or length % size:
            parser.error(f'frames file must contain a nonzero multiple of {size} bytes')
        camera += ',frames=' + qemu_path(args.frames)
    # Append our settings last so the capture helper sees only the USB camera.
    cmdline = args.append.strip() + ' console=ttyAMA0 vcam_replay.devices=0'
    command = [str(qemu), '-machine', 'virt', '-accel', args.accel,
               '-cpu', 'max' if args.accel == 'tcg' else 'host',
               '-smp', str(args.cpus), '-m', args.memory, '-nographic',
               '-kernel', str(args.kernel), '-append', cmdline.strip(),
               '-device', 'qemu-xhci,id=xhci', '-device', camera]
    if args.disk:
        command += ['-drive', f'if=none,id=rootdisk,format={args.disk_format},file={qemu_path(args.disk)}',
                    '-device', 'virtio-blk-pci,drive=rootdisk']
        if not args.writable_disk:
            command += ['-snapshot']
    if args.initrd:
        command += ['-initrd', str(args.initrd)]
    network = 'user,id=net0'
    if args.ssh_port:
        network += f',hostfwd=tcp:127.0.0.1:{args.ssh_port}-:22'
    command += ['-netdev', network, '-device', 'virtio-net-pci,netdev=net0']
    print(shlex.join(command), flush=True)
    if not args.dry_run:
        os.execv(str(qemu), command)


if __name__ == '__main__':
    try:
        main()
    except OSError as exc:
        sys.exit(f'error: {exc}')
