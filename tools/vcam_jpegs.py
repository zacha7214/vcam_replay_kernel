#!/usr/bin/env python3
"""Convert a sorted JPEG directory into GREY frames for usb-vcam (requires Pillow)."""
import argparse
import json
from pathlib import Path
import shlex
import sys
import uuid

from vcam_images import digest, geometry


def pack(directory, output, width=640, height=480):
    from PIL import Image, ImageOps

    size = geometry(width, height, 'grey')
    if not directory.is_dir():
        raise ValueError(f'not a directory: {directory}')
    paths = sorted((p for p in directory.iterdir()
                    if p.is_file() and p.suffix.lower() in ('.jpg', '.jpeg')),
                   key=lambda p: p.name)
    if not paths:
        raise ValueError('no .jpg or .jpeg files found (subdirectories are not scanned)')
    manifest = output.with_suffix(output.suffix + '.json')
    if output.exists() or manifest.exists():
        raise ValueError(f'output or manifest already exists: {output}; choose a new output')
    hashes = []
    created_raw = created_manifest = False
    try:
        with output.open('xb') as stream:
            created_raw = True
            for path in paths:
                try:
                    with Image.open(path) as source:
                        if source.format != 'JPEG':
                            raise ValueError('file is not a JPEG')
                        # Honor camera orientation; preserve aspect ratio with black bars.
                        frame = ImageOps.pad(ImageOps.exif_transpose(source).convert('L'),
                                             (width, height), method=Image.Resampling.LANCZOS,
                                             color=0, centering=(0.5, 0.5)).tobytes()
                except (OSError, ValueError) as exc:
                    raise ValueError(f'{path}: {exc}') from exc
                if len(frame) != size:
                    raise ValueError(f'{path}: unexpected decoded frame size')
                stream.write(frame)
                hashes.append(digest(frame))
        with manifest.open('x') as stream:
            created_manifest = True
            json.dump(dict(width=width, height=height, format='grey', frame_bytes=size,
                           sha256=hashes, sources=[p.name for p in paths],
                           resize='EXIF orientation, grayscale, aspect-preserving black padding'),
                      stream, indent=2)
            stream.write('\n')
    except BaseException:
        # Never leave a truncated sequence that looks ready to launch.
        if created_raw:
            output.unlink()
        if created_manifest:
            manifest.unlink()
        raise
    return len(paths)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path, help='JPEG files in filename order; zero-pad numbers')
    parser.add_argument('--output', type=Path, help='default: unique /tmp/vcam-frames-*.raw; never overwrite')
    parser.add_argument('--width', type=int, default=640)
    parser.add_argument('--height', type=int, default=480)
    parser.add_argument('--fps', type=int, default=30, help='playback rate printed for the launcher')
    args = parser.parse_args()
    if not 1 <= args.fps <= 100000:
        parser.error('fps must be 1..100000')
    output = (args.output or Path('/tmp') / f'vcam-frames-{uuid.uuid4().hex[:12]}.raw').expanduser().absolute()
    count = pack(args.directory.expanduser(), output, args.width, args.height)
    print(f'Packed {count} JPEGs into {output} ({output.stat().st_size} bytes)')
    print(f'Manifest: {output}.json')
    print('Add to scripts/run-qemu.py:')
    print(shlex.join(['--frames', str(output), '--width', str(args.width),
                      '--height', str(args.height), '--format', 'grey', '--fps', str(args.fps)]))
    print('QEMU loops this sequence. GREY is grayscale, not color.')


if __name__ == '__main__':
    try:
        main()
    except ModuleNotFoundError as exc:
        if exc.name != 'PIL':
            raise
        sys.exit('error: Pillow is required. Install in a venv: python3 -m pip install Pillow')
    except (OSError, ValueError) as exc:
        sys.exit(f'error: {exc}')
