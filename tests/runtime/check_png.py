#!/usr/bin/env python3
"""Decodes a chart PNG with an independent implementation (zlib).

The runtime's PNG encoder is written for this project, so its output is
checked here against Python's zlib rather than against itself.

Usage: check_png.py <file.png> <width> <height> <minimum bar colors>
"""

import struct
import sys
import zlib


def main() -> int:
    path, width, height, min_bars = (
        sys.argv[1],
        int(sys.argv[2]),
        int(sys.argv[3]),
        int(sys.argv[4]),
    )
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        print("bad signature")
        return 1

    pos = 8
    palette = b""
    compressed = b""
    header = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        kind = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        (crc,) = struct.unpack(">I", data[pos + 8 + length : pos + 12 + length])
        if zlib.crc32(kind + body) != crc:
            print(f"bad CRC in {kind!r}")
            return 1
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"IDAT":
            compressed += body
        pos += 12 + length

    if header != (width, height, 8, 3, 0, 0, 0):
        print(f"unexpected header {header}")
        return 1

    raw = zlib.decompress(compressed)  # Also verifies the Adler-32 checksum.
    if len(raw) != (width + 1) * height:
        print(f"decoded {len(raw)} bytes, expected {(width + 1) * height}")
        return 1

    colors = len(palette) // 3
    used = set()
    for y in range(height):
        row = raw[y * (width + 1) : (y + 1) * (width + 1)]
        if row[0] != 0:
            print(f"row {y} uses filter {row[0]}")
            return 1
        used.update(row[1:])
    if max(used) >= colors:
        print("pixel refers to a color outside the palette")
        return 1

    # Palette layout: 0 background, 1 text, 2 axis, 3 grid, 4+ bars.
    missing = [name for index, name in enumerate(["background", "text", "axis", "grid"]) if index not in used]
    if missing:
        print(f"chart has no {', '.join(missing)} pixels")
        return 1
    bars = len([index for index in used if index >= 4])
    if bars < min_bars:
        print(f"chart uses {bars} bar colors, expected at least {min_bars}")
        return 1

    print(f"{path}: {width}x{height}, {len(data)} bytes, {bars} bar colors, zlib ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
