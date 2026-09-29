#!/usr/bin/env python3
"""Install the replay driver into a Linux source tree and resolve built-in config."""
import argparse
import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from datetime import datetime, timezone

REPO = Path(__file__).resolve().parents[1]
# Hidden videobuf2 symbols are selected by our Kconfig, not forced by editing .config.
REQUESTED = ('MEDIA_SUPPORT', 'MEDIA_CAMERA_SUPPORT', 'VIDEO_DEV', 'USB_SUPPORT',
             'USB', 'PCI', 'USB_XHCI_HCD', 'USB_XHCI_PCI', 'I2C',
             'DEVTMPFS', 'DEVTMPFS_MOUNT', 'HIGH_RES_TIMERS', 'VIDEO_VCAM_REPLAY')
REQUIRED = REQUESTED + ('VIDEOBUF2_CORE', 'VIDEOBUF2_V4L2', 'VIDEOBUF2_MEMOPS',
                        'VIDEOBUF2_VMALLOC', 'DMA_SHARED_BUFFER')


def config_values(path):
    return dict(line.split('=', 1) for line in path.read_text().splitlines()
                if line.startswith('CONFIG_') and '=' in line)


def verify(path):
    values = config_values(path)
    bad = [f'CONFIG_{key}={values.get("CONFIG_" + key, "unset")}'
           for key in REQUIRED if values.get('CONFIG_' + key) != 'y']
    if bad:
        raise RuntimeError('Required built-ins did not survive Kconfig resolution:\n  ' +
                           '\n  '.join(bad) +
                           '\nCheck the target architecture and Kconfig dependencies.')


def append_once(path, line):
    text = path.read_text()
    if line not in text.splitlines():
        path.write_text(text.rstrip() + '\n\n' + line + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path, help='full Linux source tree, not headers')
    parser.add_argument('--output', type=Path, help='existing/new O= build directory')
    parser.add_argument('--arch', default=os.environ.get('ARCH'), help='e.g. arm64 or x86')
    parser.add_argument('--cross-compile', default=os.environ.get('CROSS_COMPILE'))
    parser.add_argument('--defconfig', help='initialize only when .config is missing, e.g. defconfig')
    parser.add_argument('--check', action='store_true', help='verify resolved config and installed sources; do not modify')
    parser.add_argument('--rootfs-busybox', type=Path,
                        help='also package a static ARM64 BusyBox as OUTPUT/initramfs.cpio.gz')
    args = parser.parse_args()
    rootfs = None
    if args.rootfs_busybox:
        if args.check or args.arch != 'arm64':
            parser.error('--rootfs-busybox requires --arch arm64 and cannot be used with --check')
        spec = importlib.util.spec_from_file_location('build_rootfs', Path(__file__).with_name('build-rootfs.py'))
        rootfs = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(rootfs)
        busybox_data = rootfs.validate_busybox(args.rootfs_busybox.expanduser())
    source = args.kernel.resolve()
    output = args.output.resolve() if args.output else source
    config = output / '.config'
    rootfs_path = output / 'initramfs.cpio.gz'
    if rootfs and rootfs_path.exists():
        parser.error(f'{rootfs_path} already exists; move it aside before rebuilding the rootfs')
    for name in ('Makefile', 'Kconfig', 'scripts/config', 'drivers/media/Kconfig',
                 'drivers/media/Makefile', 'include/media/videobuf2-v4l2.h'):
        if not (source / name).is_file():
            parser.error(f'{source} is not a complete Linux source tree: missing {name}')
    if os.environ.get('KCONFIG_CONFIG'):
        parser.error('KCONFIG_CONFIG is unsupported; use .config in --output instead')
    target = source / 'drivers/media/vcam-replay'
    # Explicit list: an external build leaves generated *.mod.c in driver/.
    files = [REPO / 'driver' / name for name in (
        'vcam_main.c', 'vcam_core.c', 'vcam_chardev.c', 'vcam_usb.c',
        'vcam.h', 'vcam_uapi.h', 'Kconfig', 'Kbuild')]
    hooks = [(source / 'drivers/media/Kconfig', 'source "drivers/media/vcam-replay/Kconfig"'),
             (source / 'drivers/media/Makefile', 'obj-$(CONFIG_VIDEO_VCAM_REPLAY) += vcam-replay/')]
    if args.check:
        verify(config)
        for src in files:
            if not (target / src.name).exists() or src.read_bytes() != (target / src.name).read_bytes():
                raise RuntimeError(f'Missing or stale installed source: {target / src.name}')
        for path, hook in hooks:
            if path.read_text().splitlines().count(hook) != 1:
                raise RuntimeError(f'Missing or duplicate integration hook in {path}')
        print(f'PASS: installed sources and all {len(REQUIRED)} built-ins in {config}')
        return
    if not config.exists() and not args.defconfig:
        parser.error(f'{config} is missing; pass --defconfig defconfig (or a board defconfig)')
    make = [os.environ.get('MAKE', 'make'), '-C', str(source)]
    if output != source:
        make += [f'O={output}']
    if args.arch:
        make += [f'ARCH={args.arch}']
    if args.cross_compile is not None:
        make += [f'CROSS_COMPILE={args.cross_compile}']
    # Preserve everything we replace so manual rollback is straightforward.
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    backup = source / '.vcam-backups' / stamp
    backup.mkdir(parents=True)
    for path, _ in hooks:
        shutil.copy2(path, backup / path.name)
    if target.exists():
        shutil.copytree(target, backup / 'vcam-replay')
    if config.exists():
        shutil.copy2(config, backup / 'config')
    print(f'Backup: {backup}', flush=True)
    target.mkdir(parents=True, exist_ok=True)
    for src in files:
        shutil.copy2(src, target / src.name)
    for path, hook in hooks:
        append_once(path, hook)
    if not config.exists():
        output.mkdir(parents=True, exist_ok=True)
        subprocess.run(make + [args.defconfig], check=True)
    command = [str(source / 'scripts/config'), '--file', str(config)]
    for symbol in dict.fromkeys(REQUESTED + (rootfs.REQUIRED if rootfs else ())):
        command += ['--enable', symbol]
    subprocess.run(command, check=True)
    subprocess.run(make + ['olddefconfig'], check=True)
    verify(config)
    if rootfs:
        rootfs.verify_config(config)
        with rootfs_path.open('xb') as stream:
            stream.write(rootfs.archive(busybox_data))
        print(f'Rootfs: {rootfs_path} (pass to run-qemu.py --initrd; serial shell, no SSH)')
    print(f'PASS: all {len(REQUIRED)} required options are built in: {config}')
    print('Build the complete kernel (not just modules_prepare):')
    print('  ' + shlex.join(make + ['-j' + str(os.cpu_count() or 2)]))
    print('Boot that kernel with: vcam_replay.devices=0')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(f'error: {exc}')
