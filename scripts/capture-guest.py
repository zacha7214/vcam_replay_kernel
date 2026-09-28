#!/usr/bin/env python3
"""Capture USB replay frames in the Linux guest, with a timeout and optional verification."""
import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--device', help='default: discover the unique vcam-usb video node')
    parser.add_argument('--count', type=int, default=30)
    parser.add_argument('--timeout', type=float, default=20)
    parser.add_argument('--manifest', type=Path, help='source .raw.json for byte-for-byte verification')
    args = parser.parse_args()
    if args.count < 1 or args.timeout <= 0:
        parser.error('count and timeout must be positive')
    device = args.device
    if not device:
        nodes = [p.parent.name for p in Path('/sys/class/video4linux').glob('video*/name')
                 if p.read_text().strip() == 'vcam-usb']
        if len(nodes) != 1:
            parser.error(f'found {len(nodes)} USB replay cameras; specify --device /dev/videoN')
        device = '/dev/' + nodes[0]
    # Passing an already-open stdout avoids v4l2-ctl truncating an existing file.
    with args.output.open('xb') as output:
        subprocess.run(['v4l2-ctl', '-d', device, '--stream-mmap=4',
                        f'--stream-count={args.count}', '--stream-to=-'],
                       stdout=output, check=True, timeout=args.timeout)
    if not args.output.stat().st_size:
        raise RuntimeError('capture is empty')
    if args.manifest:
        tool = Path(__file__).resolve().parents[1] / 'tools/vcam_images.py'
        subprocess.run([sys.executable, str(tool), 'verify', str(args.manifest),
                        str(args.output)], check=True)
    print(f'Captured to {args.output}; repeat with a new output name to test STREAMOFF/STREAMON')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        sys.exit(f'error: {exc}')
