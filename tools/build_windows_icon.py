#!/usr/bin/env python3
"""Generate the project-owned Windows Builder icon without external packages."""

from __future__ import annotations

import math
import struct
import sys
import zlib
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
DESTINATION = ROOT / "src/windows/builder.ico"
SIZES = (16, 24, 32, 48, 64, 128, 256)


def coverage(distance: float) -> float:
    return max(0.0, min(1.0, 0.5 - distance))


def rounded_distance(x: float, y: float, cx: float, cy: float,
                     half_width: float, half_height: float, radius: float) -> float:
    dx = abs(x - cx) - half_width + radius
    dy = abs(y - cy) - half_height + radius
    return min(max(dx, dy), 0.0) + math.hypot(max(dx, 0.0), max(dy, 0.0)) - radius


def inside_polygon(x: float, y: float, vertices: tuple[tuple[float, float], ...]) -> bool:
    inside = False
    previous_x, previous_y = vertices[-1]
    for next_x, next_y in vertices:
        if (next_y > y) != (previous_y > y):
            cross = (previous_x - next_x) * (y - next_y) / (previous_y - next_y) + next_x
            if x < cross:
                inside = not inside
        previous_x, previous_y = next_x, next_y
    return inside


def composite(base: tuple[float, float, float, float],
              rgb: tuple[int, int, int], opacity: float
              ) -> tuple[float, float, float, float]:
    r, g, b, alpha = base
    a = max(0.0, min(1.0, opacity))
    combined = a + alpha * (1.0 - a)
    if combined <= 0.0:
        return 0.0, 0.0, 0.0, 0.0
    factor = alpha * (1.0 - a)
    return ((rgb[0] * a + r * factor) / combined,
            (rgb[1] * a + g * factor) / combined,
            (rgb[2] * a + b * factor) / combined, combined)


# The gold upward transfer arrow and the cyan device form a readable 16 px icon.
ARROW = ((113, 174), (145, 141), (129, 141), (129, 110),
         (161, 110), (161, 141), (178, 141), (145, 175), (129, 191))


def render_pixel(x: float, y: float) -> tuple[int, int, int, int]:
    shade = y / 256.0
    background = (round(14 + shade * 6), round(29 + shade * 14),
                  round(51 + shade * 19))
    corner = coverage(rounded_distance(x, y, 128, 128, 124, 124, 43))
    result = composite((0.0, 0.0, 0.0, 0.0), background, corner)
    frame = coverage(rounded_distance(x, y, 125, 119, 65, 91, 18))
    result = composite(result, (88, 185, 218), frame)
    inner = coverage(rounded_distance(x, y, 125, 117, 56, 80, 12))
    result = composite(result, (24, 57, 83), inner)
    display = coverage(rounded_distance(x, y, 125, 110, 43, 59, 7))
    result = composite(result, (35, 78, 103), display)
    speaker = coverage(rounded_distance(x, y, 125, 44, 18, 3, 3))
    result = composite(result, (194, 224, 224), speaker)
    home = coverage(rounded_distance(x, y, 125, 185, 12, 4, 4))
    result = composite(result, (188, 221, 228), home)
    # An AA polygon lets the icon remain legible at 16x16 in Explorer.
    if inside_polygon(x, y, ARROW):
        result = composite(result, (255, 193, 39), 1.0)
    r, g, b, a = result
    return round(r), round(g), round(b), round(a * 255)


def base_image() -> bytes:
    rgba = bytearray(256 * 256 * 4)
    for y in range(256):
        for x in range(256):
            offset = (y * 256 + x) * 4
            rgba[offset:offset + 4] = bytes(render_pixel(x + 0.5, y + 0.5))
    return bytes(rgba)


def downsample(data: bytes, size: int) -> bytes:
    if size == 256:
        return data
    rgba = bytearray(size * size * 4)
    # Use area averaging in premultiplied alpha to avoid dark outlines.
    for y in range(size):
        y0 = y * 256 // size
        y1 = (y + 1) * 256 // size
        for x in range(size):
            x0 = x * 256 // size
            x1 = (x + 1) * 256 // size
            accum = [0, 0, 0, 0]
            for py in range(y0, y1):
                for px in range(x0, x1):
                    offset = (py * 256 + px) * 4
                    alpha = data[offset + 3]
                    for channel in range(3):
                        accum[channel] += data[offset + channel] * alpha
                    accum[3] += alpha
            pixels = (x1 - x0) * (y1 - y0)
            pos = (y * size + x) * 4
            alpha = (accum[3] + pixels // 2) // pixels
            for channel in range(3):
                rgba[pos + channel] = round(accum[channel] / accum[3]) if accum[3] else 0
            rgba[pos + 3] = alpha
    return bytes(rgba)


def png_chunk(kind: bytes, content: bytes) -> bytes:
    data = kind + content
    return struct.pack(">I", len(content)) + data + struct.pack(">I", zlib.crc32(data))


def encode_png(size: int, rgba: bytes) -> bytes:
    scanlines = bytearray()
    for y in range(size):
        scanlines.append(0)
        scanlines.extend(rgba[y * size * 4:(y + 1) * size * 4])
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" +
            png_chunk(b"IHDR", ihdr) +
            png_chunk(b"IDAT", zlib.compress(bytes(scanlines), 9)) +
            png_chunk(b"IEND", b""))


def main() -> None:
    data = base_image()
    frames = [encode_png(size, downsample(data, size)) for size in SIZES]
    offset = 6 + 16 * len(frames)
    entries = []
    for size, frame in zip(SIZES, frames):
        ico_size = 0 if size == 256 else size
        entries.append(struct.pack("<BBBBHHII", ico_size, ico_size, 0, 0, 1, 32,
                                   len(frame), offset))
        offset += len(frame)
    DESTINATION.write_bytes(struct.pack("<HHH", 0, 1, len(frames)) +
                            b"".join(entries) + b"".join(frames))
    print(f"Generated {DESTINATION.relative_to(ROOT)} with {len(frames)} sizes, "
          f"{DESTINATION.stat().st_size} bytes")


if __name__ == "__main__":
    main()
