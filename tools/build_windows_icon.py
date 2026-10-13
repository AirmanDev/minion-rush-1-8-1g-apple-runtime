#!/usr/bin/env python3
"""Package the canonical macOS installer artwork as a Windows multi-size ICO.

The graphics live in prepare_installer_icon.swift. Windows does not have a
second drawing implementation or a different icon design.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
ICON = ROOT / "src/windows/builder.ico"
SIZES = (16, 24, 32, 48, 64, 128, 256)
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def png_dimensions(blob: bytes) -> tuple[int, int]:
    if len(blob) < 24 or blob[:8] != PNG_MAGIC or blob[12:16] != b"IHDR":
        raise ValueError("invalid icon PNG")
    return struct.unpack_from(">II", blob, 16)


def render_mac_iconset(workspace: Path) -> Path:
    if platform.system() != "Darwin":
        raise RuntimeError("Icon generation requires the canonical macOS AppKit drawing source.")
    destination = workspace / "Installer.iconset"
    subprocess.run(
        ["xcrun", "swift", "-module-cache-path", str(workspace / "modules"),
         str(ROOT / "tools/prepare_installer_icon.swift"), str(destination)],
        check=True, cwd=ROOT, timeout=180,
    )
    return destination


def resample(source: Path, destination: Path, size: int) -> None:
    subprocess.run(
        ["sips", "-s", "format", "png", "-z", str(size), str(size),
         str(source), "--out", str(destination)],
        check=True, stdout=subprocess.DEVNULL, timeout=30,
    )


def encode_icon(iconset: Path, workspace: Path) -> bytes:
    images = {
        16: iconset / "icon_16x16.png",
        32: iconset / "icon_32x32.png",
        64: iconset / "icon_32x32@2x.png",
        128: iconset / "icon_128x128.png",
        256: iconset / "icon_256x256.png",
    }
    images[24] = workspace / "icon_24x24.png"
    images[48] = workspace / "icon_48x48.png"
    resample(images[32], images[24], 24)
    resample(images[64], images[48], 48)
    entries = []
    frames = []
    offset = 6 + 16 * len(SIZES)
    for size in SIZES:
        blob = images[size].read_bytes()
        if png_dimensions(blob) != (size, size):
            raise ValueError(f"source icon size mismatch at {size}")
        entry_size = size if size < 256 else 0
        entries.append(struct.pack("<BBBBHHII", entry_size, entry_size, 0, 0,
                                   1, 32, len(blob), offset))
        frames.append(blob)
        offset += len(blob)
    return struct.pack("<HHH", 0, 1, len(SIZES)) + b"".join(entries) + b"".join(frames)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true",
                        help="compare the existing Windows icon to freshly rendered macOS artwork")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="mr-shared-icon-") as temporary:
        working = Path(temporary)
        data = encode_icon(render_mac_iconset(working), working)
    if args.verify:
        if ICON.read_bytes() != data:
            raise SystemExit("Windows icon differs from the canonical macOS icon source.")
        print("Windows/macOS icon source is identical:", hashlib.sha256(data).hexdigest())
        return
    ICON.write_bytes(data)
    print("Generated from macOS installer icon:", ICON.relative_to(ROOT),
          "sizes:", ",".join(map(str, SIZES)), "bytes:", len(data))


if __name__ == "__main__":
    main()
