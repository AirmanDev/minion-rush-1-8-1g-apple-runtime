#!/usr/bin/env python3
"""Encode local AOT metadata for an unsigned, engine-independent iOS runtime.

The public template is compiled without these values. The user-owned source
archive supplies them on the host, before the IPA is signed or installed.
"""
from __future__ import annotations

import hashlib
import json
import re
import struct
from pathlib import Path

MAGIC = b"MRPROF3\x00"
VERSION = 3
HEADER = struct.Struct("<8sIQIIIIII32s")
BLOCK = struct.Struct("<IIB3x")
CONFIG = json.loads((Path(__file__).resolve().parents[1] /
                     "config/native_profile.json").read_text(encoding="ascii"))
if (set(CONFIG) != {"version", "code_slot_bytes", "max_blocks", "max_blocked", "max_hooks"}
        or CONFIG["version"] != VERSION):
    raise ValueError("invalid native profile configuration")
MAX_BLOCKS = CONFIG["max_blocks"]
MAX_BLOCKED = CONFIG["max_blocked"]
MAX_HOOKS = CONFIG["max_hooks"]
MAX_CODE_BYTES = CONFIG["code_slot_bytes"]
HASH_DEFINE = re.compile(r"^#define MR_GAME_(BINDING|BLOCK)_HASH UINT64_C\(0x([0-9a-fA-F]{16})\)$")
VALUE_DEFINE = re.compile(r"^#define MR_GAME_([A-Z0-9_]+) 0x([0-9a-fA-F]{8})u$")
COUNT_DEFINE = re.compile(
    r"^#define MR_GAME_(SYMBOL|EXACT_BLOCKED_FUNCTION|BLOCKED_FUNCTION|OFFLINE_HOOK_FUNCTION)_COUNT ([0-9]+)u$"
)
BLOCK_LINE = re.compile(r"^(A32|THUMB)_BLOCK\(0x([0-9a-fA-F]{8})u, ([0-9]+)u\)$")
ARRAY = re.compile(r"static const uint32_t (MR_GAME_(?:BLOCKED|OFFLINE_HOOK)_FUNCTIONS)\[\] = \{(.*?)\};", re.S)
HEX_VALUE = re.compile(r"0x([0-9a-fA-F]{8})u")


def symbol_names(definitions: Path) -> tuple[str, ...]:
    # The source definition is an audited, public interface description.
    # Lines may be wrapped before their quoted C++ mangled symbol name.
    content = definitions.read_text(encoding="ascii")
    names = re.findall(r"MR_GAME_(?:HOOK_)?SYMBOL\(\s*([A-Z0-9_]+)\s*,", content)
    if not names or len(names) != len(set(names)):
        raise ValueError("duplicate or missing host binding names")
    return tuple(names)


def _parse_bindings(path: Path, names: tuple[str, ...]) -> tuple[int, list[int], list[int], list[int], int]:
    source = path.read_text(encoding="ascii")
    values: dict[str, int] = {}
    counts: dict[str, int] = {}
    hashes: dict[str, int] = {}
    for line in source.splitlines():
        if m := VALUE_DEFINE.fullmatch(line):
            if m[1] in values:
                raise ValueError("duplicate game binding")
            values[m[1]] = int(m[2], 16)
        elif m := COUNT_DEFINE.fullmatch(line):
            counts[m[1]] = int(m[2])
        elif m := HASH_DEFINE.fullmatch(line):
            hashes[m[1]] = int(m[2], 16)
    if set(values) != set(names) or counts.get("SYMBOL") != len(names):
        raise ValueError("generated binding definitions differ from public interface")
    if not hashes.get("BINDING"):
        raise ValueError("missing engine fingerprint")
    found: dict[str, list[int]] = {}
    for m in ARRAY.finditer(source):
        text = m[2].strip()
        numbers = HEX_VALUE.findall(text)
        if re.sub(r"0x[0-9a-fA-F]{8}u\s*,?", "", text).strip():
            raise ValueError("invalid address in engine hook array")
        if m[1] in found:
            raise ValueError("duplicate engine hook array")
        found[m[1]] = [int(n, 16) for n in numbers]
    if set(found) != {"MR_GAME_BLOCKED_FUNCTIONS", "MR_GAME_OFFLINE_HOOK_FUNCTIONS"}:
        raise ValueError("missing generated hook arrays")
    blocked, hooks = found["MR_GAME_BLOCKED_FUNCTIONS"], found["MR_GAME_OFFLINE_HOOK_FUNCTIONS"]
    if (not 1 <= len(blocked) <= MAX_BLOCKED or not len(blocked) < len(hooks) <= MAX_HOOKS
            or counts.get("EXACT_BLOCKED_FUNCTION") is None
            or counts.get("BLOCKED_FUNCTION") != len(blocked)
            or counts.get("OFFLINE_HOOK_FUNCTION") != len(hooks)
            or counts["EXACT_BLOCKED_FUNCTION"] >= len(blocked)):
        raise ValueError("inconsistent blocked-function and hook counts")
    if blocked != sorted(set(blocked)) or hooks != sorted(set(hooks)):
        raise ValueError("hook address order or uniqueness is invalid")
    return hashes["BINDING"], [values[n] for n in names], blocked, hooks, counts["EXACT_BLOCKED_FUNCTION"]


def _parse_map(path: Path, fingerprint: int, code_size: int) -> list[tuple[int, int, int]]:
    blocks = []
    last = [-1, -1]
    block_hash = None
    for line in path.read_text(encoding="ascii").splitlines():
        if not line or line.startswith("//"):
            continue
        if m := HASH_DEFINE.fullmatch(line):
            if m[1] != "BLOCK" or block_hash is not None:
                raise ValueError("invalid block fingerprint declaration")
            block_hash = int(m[2], 16)
            continue
        m = BLOCK_LINE.fullmatch(line)
        if not m:
            raise ValueError("malformed generated AOT block map")
        thumb = int(m[1] == "THUMB")
        pc, offset = int(m[2], 16), int(m[3])
        if pc <= last[thumb] or offset >= code_size or offset % 4:
            raise ValueError("invalid block address or native code offset")
        last[thumb] = pc
        blocks.append((pc, offset, thumb))
        if len(blocks) > MAX_BLOCKS:
            raise ValueError("AOT block map exceeds capacity")
    if block_hash != fingerprint or not blocks:
        raise ValueError("AOT map and binding fingerprints differ")
    return blocks


def create_profile(compiled: Path, symbols: Path) -> bytes:
    names = symbol_names(symbols)
    code = compiled / "minion_code.bin"
    code_size = code.stat().st_size
    if not 64 <= code_size <= MAX_CODE_BYTES or code_size % 4:
        raise ValueError("native ARM64 code exceeds the generic template slot")
    fingerprint, bindings, blocked, hooks, exact = _parse_bindings(
        compiled / "game_bindings.h", names)
    blocks = _parse_map(compiled / "minion_map.inc", fingerprint, code_size)
    data = bytearray()
    for value in bindings:
        data.extend(struct.pack("<I", value))
    for pc, offset, thumb in blocks:
        data.extend(BLOCK.pack(pc, offset, thumb))
    for value in blocked + hooks:
        data.extend(struct.pack("<I", value))
    digest = hashlib.sha256(data).digest()
    return HEADER.pack(MAGIC, VERSION, fingerprint, code_size, len(bindings),
                       len(blocks), len(blocked), len(hooks), exact, digest) + data


def inspect_profile(data: bytes, symbols: Path) -> dict:
    if len(data) < HEADER.size:
        raise ValueError("truncated AOT profile")
    magic, version, fingerprint, code_size, names, blocks, blocked, hooks, exact, digest = HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION or names != len(symbol_names(symbols)):
        raise ValueError("unsupported AOT profile version or binding ABI")
    if (not 64 <= code_size <= MAX_CODE_BYTES or code_size % 4
            or not 1 <= blocks <= MAX_BLOCKS or not 1 <= blocked <= MAX_BLOCKED
            or not blocked < hooks <= MAX_HOOKS or exact >= blocked):
        raise ValueError("invalid profile sizes")
    expected = HEADER.size + 4 * (names + blocked + hooks) + BLOCK.size * blocks
    if len(data) != expected or hashlib.sha256(data[HEADER.size:]).digest() != digest:
        raise ValueError("AOT profile length or checksum mismatch")
    return {"fingerprint": fingerprint, "code_size": code_size,
            "symbols": names, "blocks": blocks, "blocked": blocked, "hooks": hooks}


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--code-slot-bytes", action="store_true", required=True)
    parser.parse_args()
    print(MAX_CODE_BYTES)
