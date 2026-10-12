"""Build iOS bundle icons from the user's validated original launcher PNG.

The unsigned template contains no app icon. Converted images remain only in
the private per-user IPA. PNG parsing is restricted to the official RGBA input.
"""

from __future__ import annotations

import plistlib
import struct
import zlib
from pathlib import Path

from validate_assets import validate_app_icon

SOURCE_SIZE = 576
ICON_DIMENSIONS = {
    "MRIcon60@2x.png": 120,
    "MRIcon60@3x.png": 180,
    "MRIcon76@2x.png": 152,
    "MRIcon83.5@2x.png": 167,
}


def _chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


def _read_rgba(png: bytes) -> bytearray:
    if not png.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("invalid original app icon PNG")
    offset = 8
    compressed = bytearray()
    dimensions = None
    ended = False
    while offset < len(png):
        if offset + 12 > len(png):
            raise ValueError("truncated original PNG chunk")
        size = struct.unpack_from(">I", png, offset)[0]
        if size > 4 * 1024 * 1024 or offset + 12 + size > len(png):
            raise ValueError("invalid original PNG chunk size")
        tag = png[offset + 4:offset + 8]
        data = png[offset + 8:offset + 8 + size]
        expected_crc = struct.unpack_from(">I", png, offset + 8 + size)[0]
        if zlib.crc32(tag + data) != expected_crc:
            raise ValueError("invalid original PNG checksum")
        if tag == b"IHDR":
            if dimensions is not None or size != 13:
                raise ValueError("invalid original PNG header")
            dimensions = struct.unpack(">IIBBBBB", data)
            if dimensions != (SOURCE_SIZE, SOURCE_SIZE, 8, 6, 0, 0, 0):
                raise ValueError("unexpected original PNG pixel format")
        elif tag == b"IDAT":
            compressed.extend(data)
            if len(compressed) > 4 * 1024 * 1024:
                raise ValueError("original PNG exceeds compressed size limit")
        elif tag == b"IEND":
            ended = True
            if size != 0 or offset + 12 != len(png):
                raise ValueError("unexpected data after original PNG")
            break
        elif tag[0] & 0x20 == 0:
            raise ValueError("unsupported critical PNG chunk")
        offset += 12 + size
    if dimensions is None or not ended:
        raise ValueError("incomplete original PNG")
    stride = SOURCE_SIZE * 4
    expected = SOURCE_SIZE * (stride + 1)
    inflater = zlib.decompressobj()
    decoded = inflater.decompress(bytes(compressed), expected + 1)
    if len(decoded) != expected or not inflater.eof or inflater.unused_data:
        raise ValueError("invalid original PNG decompressed size")
    pixels = bytearray(SOURCE_SIZE * stride)
    previous = bytearray(stride)
    for y in range(SOURCE_SIZE):
        start = y * (stride + 1)
        filter_type = decoded[start]
        row = bytearray(decoded[start + 1:start + stride + 1])
        if filter_type > 4:
            raise ValueError("unsupported original PNG row filter")
        for i in range(stride):
            a = row[i - 4] if i >= 4 else 0
            b = previous[i]
            c = previous[i - 4] if i >= 4 else 0
            if filter_type == 1:
                row[i] = (row[i] + a) & 255
            elif filter_type == 2:
                row[i] = (row[i] + b) & 255
            elif filter_type == 3:
                row[i] = (row[i] + (a + b) // 2) & 255
            elif filter_type == 4:
                estimate = a + b - c
                pa, pb, pc = abs(estimate-a), abs(estimate-b), abs(estimate-c)
                predictor = a if pa <= pb and pa <= pc else b if pb <= pc else c
                row[i] = (row[i] + predictor) & 255
        pixels[y * stride:(y + 1) * stride] = row
        previous = row
    return pixels


def _flatten_launcher(rgba: bytearray) -> bytes:
    """Remove the old red launcher border as in the normal Mac icon build."""
    size = SOURCE_SIZE
    count = size * size
    inset = size * 9 // 100
    radius = size // 8

    def outside_red(index: int) -> bool:
        i = index * 4
        alpha = rgba[i + 3]
        return alpha == 0 or (
            rgba[i] >= 200 and rgba[i + 1] <= 150 and rgba[i + 2] <= 130
        )

    visited = bytearray(count)
    queue: list[int] = []

    def enqueue(index: int) -> None:
        if not visited[index] and outside_red(index):
            visited[index] = 1
            queue.append(index)

    for x in range(size):
        enqueue(x)
        enqueue((size - 1) * size + x)
    for y in range(size):
        enqueue(y * size)
        enqueue(y * size + size - 1)
    head = 0
    while head < len(queue):
        index = queue[head]
        head += 1
        x, y = index % size, index // size
        if x:
            enqueue(index - 1)
        if x + 1 < size:
            enqueue(index + 1)
        if y:
            enqueue(index - size)
        if y + 1 < size:
            enqueue(index + size)

    rgb = bytearray(count * 3)
    for index in range(count):
        x, y = index % size, index // size
        cx = max(inset + radius, min(size - inset - 1 - radius, x))
        cy = max(inset + radius, min(size - inset - 1 - radius, y))
        outside_card = (x - cx) ** 2 + (y - cy) ** 2 > radius * radius
        protected = False
        if outside_card and (y < inset or y >= size - inset):
            direction = 1 if y < inset else -1
            evidence = 0
            for step in range(1, 9):
                probe_y = y + step * direction
                if 0 <= probe_y < size:
                    position = 4 * (probe_y * size + x)
                    if (rgba[position + 3] > 0
                            and min(rgba[position:position + 3]) < 210
                            and not outside_red(probe_y * size + x)):
                        evidence += 1
            protected = evidence >= 4
        dest = index * 3
        src = index * 4
        if visited[index] or (outside_card and (not protected or outside_red(index))):
            rgb[dest:dest + 3] = b"\xff\xff\xff"
        else:
            alpha = rgba[src + 3]
            for channel in range(3):
                rgb[dest + channel] = (rgba[src + channel] * alpha + 255 * (255 - alpha) + 127) // 255
    return bytes(rgb)


def _write_png(rgb: bytes, size: int) -> bytes:
    stride = size * 3
    compressor = zlib.compressobj(level=7)
    packed = bytearray()
    for y in range(size):
        packed.extend(compressor.compress(b"\x00" + rgb[y * stride:(y + 1) * stride]))
    packed.extend(compressor.flush())
    return (
        b"\x89PNG\r\n\x1a\n"
        + _chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
        + _chunk(b"IDAT", bytes(packed))
        + _chunk(b"IEND", b"")
    )


def _resize(rgb: bytes, size: int) -> bytes:
    """Area-average pixels; output is deterministic without image libraries."""
    width = SOURCE_SIZE
    out = bytearray(size * size * 3)
    # The source is larger than every required Home Screen icon.
    for y in range(size):
        y0, y1 = y * width // size, (y + 1) * width // size
        for x in range(size):
            x0, x1 = x * width // size, (x + 1) * width // size
            totals = [0, 0, 0]
            for sy in range(y0, y1):
                for sx in range(x0, x1):
                    i = (sy * width + sx) * 3
                    totals[0] += rgb[i]
                    totals[1] += rgb[i + 1]
                    totals[2] += rgb[i + 2]
            divisor = (y1 - y0) * (x1 - x0)
            dest = (y * size + x) * 3
            for channel in range(3):
                out[dest + channel] = (totals[channel] + divisor // 2) // divisor
    return bytes(out)


def install_private_icon(app: Path, source_icon: Path) -> None:
    """Replace template icon metadata with PNGs generated from verified ZIP."""
    validate_app_icon(source_icon)
    info_path = app / "Info.plist"
    info = plistlib.loads(info_path.read_bytes())
    if not isinstance(info, dict):
        raise ValueError("invalid app Info.plist")
    # Do not ship a placeholder compiled icon catalog alongside the real PNGs.
    if (app / "Assets.car").exists():
        raise ValueError("template still contains compiled app icon assets")
    for key in ("CFBundleIcons", "CFBundleIcons~ipad", "CFBundleIconFiles", "CFBundleIconName"):
        if key in info:
            raise ValueError("template contains unexpected app icon metadata")
    pixels = _flatten_launcher(_read_rgba(source_icon.read_bytes()))
    for name, size in ICON_DIMENSIONS.items():
        destination = app / name
        if destination.exists() or destination.is_symlink():
            raise ValueError("refusing to overwrite an existing private icon")
        destination.write_bytes(_write_png(_resize(pixels, size), size))
    primary = {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["MRIcon60"]}}
    ipad = {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["MRIcon60", "MRIcon76", "MRIcon83.5"]}}
    info["CFBundleIcons"] = primary
    info["CFBundleIcons~ipad"] = ipad
    info_path.write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_BINARY, sort_keys=True))
