#!/usr/bin/env python3
"""Build the prepared ARM64 QEMU on Linux or macOS, then verify usb-vcam."""
import argparse
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys

PACKAGES = ('build-essential ninja-build pkg-config python3 python3-venv '
            'python3-pip python3-setuptools python3-wheel meson libglib2.0-dev '
            'libpixman-1-dev libslirp-dev '
            'libfdt-dev zlib1g-dev')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path.home() / 'qemu-11.1.1')
    parser.add_argument('--build-dir', type=Path, help='default: SOURCE/build-vcam')
    parser.add_argument('-j', '--jobs', type=int, default=min(os.cpu_count() or 2, 4))
    parser.add_argument('--dry-run', action='store_true', help='check integration and print commands; do not build')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('jobs must be positive')
    source = args.source.expanduser().resolve()
    build = args.build_dir.expanduser().resolve() if args.build_dir else source / 'build-vcam'
    if source == build:
        parser.error('use a separate build directory')
    subprocess.run([sys.executable, str(Path(__file__).with_name('setup-qemu.py')),
                    str(source), '--check'], check=True)
    command = [str(source / 'configure'), '--target-list=aarch64-softmmu',
               '--without-default-features', '--enable-system', '--enable-tcg',
               '--enable-slirp', '--enable-fdt', '--enable-pixman',
               '--disable-docs', '--disable-werror', '--disable-rust']
    if platform.system() == 'Linux' and platform.machine() in ('aarch64', 'arm64'):
        command.append('--enable-kvm')
    elif platform.system() == 'Darwin' and platform.machine() == 'arm64':
        command.append('--enable-hvf')
    ninja = ['ninja', '-C', str(build), '-j', str(args.jobs), 'qemu-system-aarch64']
    print('Build directory: ' + str(build), flush=True)
    print(shlex.join(command), flush=True)
    print(shlex.join(ninja), flush=True)
    if args.dry_run:
        return
    missing = [name for name in ('cc', 'make', 'ninja', 'pkg-config') if not shutil.which(name)]
    if not missing:
        for library in ('glib-2.0', 'pixman-1', 'slirp'):
            if subprocess.run(['pkg-config', '--exists', library]).returncode:
                missing.append(library)
    if missing:
        raise ValueError('missing build dependencies: ' + ', '.join(missing) +
                         '\nOn Ubuntu install:\n  sudo apt install ' + PACKAGES)
    build.mkdir(parents=True, exist_ok=True)
    subprocess.run(command, cwd=build, check=True)
    subprocess.run(ninja, check=True)
    result = subprocess.run([str(build / 'qemu-system-aarch64'), '-device', 'usb-vcam,help'],
                            check=True, capture_output=True, text=True)
    if 'frames' not in result.stdout + result.stderr:
        raise ValueError('built QEMU does not expose usb-vcam frames; check its device configuration')
    print(result.stdout + result.stderr)
    print('PASS: built QEMU exposes usb-vcam with host-file replay')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        sys.exit(f'error: {exc}')
