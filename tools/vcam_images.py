#!/usr/bin/env python3
"""Prepare host images for usb-vcam and verify raw V4L2 captures (Python 3 only)."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


def geometry(width, height, fmt):
    if not (16 <= width <= 8192 and 16 <= height <= 8192):
        raise ValueError('width and height must be 16..8192')
    if fmt == 'yuyv' and width % 2:
        raise ValueError('YUYV width must be even')
    return width * height * (2 if fmt == 'yuyv' else 1)


def read_pgm(path):
    data = path.read_bytes()
    pos = 0
    tokens = []
    while len(tokens) < 4:
        while pos < len(data):
            if data[pos] in b' \t\r\n\v\f':
                pos += 1
            elif data[pos] == ord('#'):
                end = data.find(b'\n', pos)
                if end < 0:
                    raise ValueError(f'{path}: unterminated PGM comment')
                pos = end + 1
            else:
                break
        match = re.match(rb'[^\s#]+', data[pos:])
        if not match:
            raise ValueError(f'{path}: truncated PGM header')
        tokens.append(match[0])
        pos += len(match[0])
    if tokens[0] != b'P5' or tokens[3] != b'255':
        raise ValueError(f'{path}: expected binary P5 PGM with maxval 255')
    width, height = map(int, tokens[1:3])
    size = geometry(width, height, 'grey')
    if pos >= len(data) or data[pos] not in b' \t\r\n\v\f':
        raise ValueError(f'{path}: missing raster separator')
    # Consume only the header separator: the first pixel can itself be whitespace.
    pos += 2 if data[pos:pos + 2] == b'\r\n' else 1
    pixels = data[pos:]
    if len(pixels) != size:
        raise ValueError(f'{path}: expected {size} pixels, got {len(pixels)}')
    return width, height, pixels


def digest(frame):
    return hashlib.sha256(frame).hexdigest()


def write_frames(output, width, height, fmt, frames):
    size = geometry(width, height, fmt)
    hashes = []
    # Exclusive creation avoids accidentally truncating an input or mapped QEMU file.
    with output.open('xb') as stream:
        for frame in frames:
            if len(frame) != size:
                raise ValueError(f'expected {size} bytes per frame, got {len(frame)}')
            stream.write(frame)
            hashes.append(digest(frame))
    if not hashes:
        raise ValueError('no frames')
    manifest = dict(width=width, height=height, format=fmt,
                    frame_bytes=size, sha256=hashes)
    output.with_suffix(output.suffix + '.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'{len(hashes)} frames written to {output}')
    print(f'QEMU: -device usb-vcam,bus=xhci.0,width={width},height={height},'
          f'format={fmt},fps=30,frames={output.resolve()}')


def verify(manifest_path, capture, strict=False):
    manifest = json.loads(manifest_path.read_text())
    size = geometry(manifest['width'], manifest['height'], manifest['format'])
    hashes = manifest['sha256']
    allowed = set(hashes)
    count = 0
    with capture.open('rb') as stream:
        while frame := stream.read(size):
            if len(frame) != size:
                raise ValueError(f'capture ends with a partial frame ({len(frame)}/{size} bytes)')
            actual = digest(frame)
            if actual not in allowed:
                raise ValueError(f'frame {count} differs from every source image')
            if strict and actual != hashes[count % len(hashes)]:
                raise ValueError(f'frame {count} is out of sequence (a frame may have dropped)')
            count += 1
    if not count:
        raise ValueError('empty capture')
    print(f'PASS: {count} captured frames match source images byte-for-byte'
          + (' in exact replay order' if strict else ' (drops/order not checked)'))
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    pack = commands.add_parser('pack', help='pack sorted PGM files or fixed-size raw files')
    pack.add_argument('directory', type=Path)
    pack.add_argument('output', type=Path)
    pack.add_argument('--format', choices=['grey', 'yuyv'], default='grey')
    pack.add_argument('--width', type=int)
    pack.add_argument('--height', type=int)
    pattern = commands.add_parser('pattern', help='create distinctive GREY smoke-test frames')
    pattern.add_argument('output', type=Path)
    pattern.add_argument('--width', type=int, default=640)
    pattern.add_argument('--height', type=int, default=480)
    pattern.add_argument('--count', type=int, default=1)
    check = commands.add_parser('verify', help='compare captured frames with source hashes')
    check.add_argument('manifest', type=Path)
    check.add_argument('capture', type=Path)
    check.add_argument('--strict-sequence', action='store_true', help='require frame 0 first, no drops/repeats')
    args = parser.parse_args()
    if args.command == 'verify':
        verify(args.manifest, args.capture, args.strict_sequence)
    elif args.command == 'pattern':
        geometry(args.width, args.height, 'grey')
        if not 1 <= args.count <= 256:
            parser.error('--count must be 1..256')
        frames = (bytes((x * 7 + y * 13 + n * 31 + 19) % 256
                        for y in range(args.height) for x in range(args.width))
                  for n in range(args.count))
        write_frames(args.output, args.width, args.height, 'grey', frames)
    else:
        pgms = sorted(args.directory.glob('*.pgm'))
        raws = sorted(args.directory.glob('*.raw'))
        if pgms and raws:
            parser.error('directory contains both .pgm and .raw; use a single input type')
        paths = pgms or raws
        if not paths:
            parser.error('no .pgm or .raw files found')
        if args.output.resolve() in [p.resolve() for p in paths]:
            parser.error('output must not be an input file')
        width, height = args.width, args.height
        if pgms:
            if args.format != 'grey':
                parser.error('PGM requires --format grey')
            w, h, _ = read_pgm(pgms[0])
            if (width is not None and width != w) or (height is not None and height != h):
                parser.error('requested dimensions differ from PGM')
            width, height = w, h
            # Validate all files before writing the output.
            for path in pgms:
                if read_pgm(path)[:2] != (w, h):
                    parser.error(f'{path}: dimensions differ from first PGM')
            frames = (read_pgm(path)[2] for path in pgms)
        else:
            if width is None or height is None:
                parser.error('raw input requires --width and --height')
            size = geometry(width, height, args.format)
            for path in raws:
                if path.stat().st_size != size:
                    parser.error(f'{path}: expected exactly {size} bytes')
            frames = (path.read_bytes() for path in raws)
        write_frames(args.output, width, height, args.format, frames)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError) as exc:
        sys.exit(f'error: {exc}')
