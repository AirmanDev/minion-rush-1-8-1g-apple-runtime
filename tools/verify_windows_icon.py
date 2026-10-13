#!/usr/bin/env python3
"""Check Windows PE icon resources and the maintained multi-resolution ICO."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def verify_icon(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < 22 or data[:4] != b"\x00\x00\x01\x00":
        raise ValueError("invalid Windows ICO header")
    count = struct.unpack_from("<H", data, 4)[0]
    sizes = set()
    for index in range(count):
        width, height, _palette, _reserved, planes, depth, length, offset = (
            struct.unpack_from("<BBBBHHII", data, 6 + index * 16))
        size = width or 256
        if (size != (height or 256) or size in sizes or planes != 1 or depth != 32 or
                offset + length > len(data) or length < 60 or
                not data[offset:offset + 8] == b"\x89PNG\r\n\x1a\n"):
            raise ValueError("invalid ICO image")
        sizes.add(size)
    if not {16, 32, 48, 256}.issubset(sizes):
        raise ValueError("Windows ICO is missing required resolutions")
    return count


def verify_pe_icon(path: Path) -> None:
    data = path.read_bytes()
    if data[:2] != b"MZ" or len(data) < 256:
        raise ValueError("not a Windows executable")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\x00\x00":
        raise ValueError("invalid Windows executable header")
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ValueError("expected PE32+ x64 executable")
    resource_rva, resource_length = struct.unpack_from("<II", data, optional + 112 + 16)
    if not resource_rva or resource_length < 32:
        raise ValueError("Windows executable has no resource directory")
    for index in range(sections):
        section = optional + optional_size + 40 * index
        virtual_size, virtual_address, raw_size, raw_start = (
            struct.unpack_from("<IIII", data, section + 8))
        if virtual_address <= resource_rva < virtual_address + max(virtual_size, raw_size):
            base = raw_start + resource_rva - virtual_address
            break
    else:
        raise ValueError("Windows resource directory is unmapped")
    _, _, _, _, names, ids = struct.unpack_from("<IIHHHH", data, base)
    types = {}
    for index in range(names + ids):
        resource_id, address = struct.unpack_from("<II", data, base + 16 + 8 * index)
        if resource_id & 0x80000000:
            continue
        types[resource_id] = address
    if 3 not in types or 14 not in types:
        raise ValueError("missing icon or icon-group Windows resources")
    group = types[14]
    if not group & 0x80000000:
        raise ValueError("invalid icon group resource")
    directory = base + (group & 0x7FFFFFFF)
    _, _, _, _, names, ids = struct.unpack_from("<IIHHHH", data, directory)
    identifiers = {
        struct.unpack_from("<I", data, directory + 16 + index * 8)[0]
        for index in range(names + ids)
    }
    if 101 not in identifiers:
        raise ValueError("missing application icon resource ID 101")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ico", type=Path, required=True)
    parser.add_argument("executables", nargs="+", type=Path)
    args = parser.parse_args()
    print("Windows ICO frames:", verify_icon(args.ico))
    for path in args.executables:
        verify_pe_icon(path)
        print("Icon embedded:", path)


if __name__ == "__main__":
    main()
