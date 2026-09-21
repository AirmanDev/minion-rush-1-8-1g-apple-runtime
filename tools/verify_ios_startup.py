#!/usr/bin/env python3
"""Classify an iOS startup log and validate its captured framebuffer."""

from __future__ import annotations

import struct
import sys
from pathlib import Path

FAILURE_MARKERS = (
    "STOPPED:",
    "ERROR: engine stopped",
    "ERROR: guest attempted a network import",
    "presentation buffer is black",
)
READY_MARKERS = ("startup services ready:", "startup frame check:", "PNG saved")


def startup_state(log: str) -> str:
    if any(marker in log for marker in FAILURE_MARKERS):
        return "failed"
    if all(marker in log for marker in READY_MARKERS):
        return "ready"
    return "pending"


def valid_png(path: Path, portrait: bool = False) -> bool:
    try:
        header = path.read_bytes()[:24]
    except OSError:
        return False
    valid_header = (
        len(header) == 24
        and header[:8] == b"\x89PNG\r\n\x1a\n"
        and header[12:16] == b"IHDR"
    )
    if not valid_header:
        return False
    width, height = struct.unpack(">II", header[16:24])
    return width > 0 and height > 0 and (not portrait or height > width)


def main() -> int:
    if len(sys.argv) not in (2, 3, 4) or (len(sys.argv) == 4 and sys.argv[3] != "portrait"):
        print(f"Usage: {sys.argv[0]} <log> [PNG [portrait]]", file=sys.stderr)
        return 2
    try:
        log = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return 1
    state = startup_state(log)
    if state == "failed":
        return 2
    if state == "pending":
        return 1
    if len(sys.argv) >= 3 and not valid_png(Path(sys.argv[2]), len(sys.argv) == 4):
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
