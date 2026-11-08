"""Converts the emulator's 480x480 RGB565 .raw frames to PNG (stdlib only)."""

import pathlib
import struct
import sys
import zlib

SIZE = 480


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))


def to_png(raw):
    pixels = struct.unpack(f"<{SIZE * SIZE}H", raw)
    rows = bytearray()
    for y in range(SIZE):
        rows.append(0)  # no filter
        for p in pixels[y * SIZE:(y + 1) * SIZE]:
            r, g, b = p >> 11, (p >> 5) & 0x3F, p & 0x1F
            rows += bytes(((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31))
    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))


for raw_path in pathlib.Path(sys.argv[1]).glob("*.raw"):
    raw = raw_path.read_bytes()
    assert len(raw) == SIZE * SIZE * 2, raw_path
    out = pathlib.Path(sys.argv[2]) / (raw_path.stem + ".png")
    out.write_bytes(to_png(raw))
    print(out)
