#!/usr/bin/env python3
"""Install usb-vcam into a QEMU source tree (no build or package installation)."""
import argparse
from datetime import datetime, timezone
from pathlib import Path
import re
import shutil
import sys

REPO = Path(__file__).resolve().parents[1]
KCONFIG = 'config USB_VCAM\n    bool\n    default y\n    depends on USB\n'
MESON = "system_ss.add(when: 'CONFIG_USB_VCAM', if_true: files('dev-vcam.c'))"


def plan(source):
    for name in ('configure', 'hw/usb/Kconfig', 'hw/usb/meson.build', 'include/hw/usb/usb.h'):
        if not (source / name).is_file():
            raise ValueError(f'not a QEMU source tree: missing {source / name}')
    config = source / 'hw/usb/Kconfig'
    meson = source / 'hw/usb/meson.build'
    text = config.read_text()
    entries = list(re.finditer(r'^config USB_VCAM\s*\n', text, re.M))
    if entries:
        if len(entries) != 1:
            raise ValueError('duplicate USB_VCAM entries in Kconfig')
        start = entries[0].start()
        end = re.search(r'^config ', text[entries[0].end():], re.M)
        stop = entries[0].end() + end.start() if end else len(text)
        if text[start:stop].split() != KCONFIG.split():
            raise ValueError('existing USB_VCAM Kconfig differs; review it before setup')
    else:
        text = text.rstrip() + '\n\n' + KCONFIG
    meson_text = meson.read_text()
    relevant = [line for line in meson_text.splitlines()
                if 'CONFIG_USB_VCAM' in line or 'dev-vcam.c' in line]
    if relevant and relevant != [MESON]:
        raise ValueError('existing usb-vcam Meson entry differs or is duplicated; review it')
    if not relevant:
        meson_text = meson_text.rstrip() + '\n\n' + MESON + '\n'
    return {config: text.encode(), meson: meson_text.encode(),
            source / 'hw/usb/dev-vcam.c': (REPO / 'qemu-device/dev-vcam.c').read_bytes()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', nargs='?', type=Path, default=Path.home() / 'qemu-11.1.1')
    parser.add_argument('--check', action='store_true', help='verify exact integration without modifying files')
    args = parser.parse_args()
    source = args.source.expanduser().resolve()
    changes = {p: data for p, data in plan(source).items()
               if not p.exists() or p.read_bytes() != data}
    if args.check:
        if changes:
            raise ValueError('missing/stale integration: ' + ', '.join(str(p) for p in changes))
        print(f'PASS: usb-vcam source, Kconfig and Meson hooks match in {source}')
        return
    if changes:
        stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
        backup = source / '.vcam-backups' / stamp
        backup.mkdir(parents=True)
        for path in changes:
            if path.exists():
                shutil.copy2(path, backup / path.name)
        print(f'Backup of replaced files: {backup}')
        for path, data in changes.items():
            path.write_bytes(data)
            print(f'Updated {path}')
    else:
        print('Already up to date')
    print('Ready to build with scripts/build-qemu.py --source ' + str(source))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as exc:
        sys.exit(f'error: {exc}')
