"""Strict Mach-O executable-slot discovery for privately assembled iOS templates.

Only thin ARM64 MH_EXECUTE files with one explicitly mapped __MRCODE/__text
section are accepted. No app executable data is executed by this parser.
"""
from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass
from pathlib import Path

_HEADER = struct.Struct("<IiiIIIII")
_SEGMENT = struct.Struct("<II16sQQQQiiII")
_SECTION = struct.Struct("<16s16sQQIIIIIIII")
MAGIC_64 = 0xFEEDFACF
ARM64 = 0x0100000C
MH_EXECUTE = 2
LC_SEGMENT_64 = 0x19
LC_CODE_SIGNATURE = 0x1D
VM_PROT_EXECUTE = 0x4
MIN_SLOT = 64
MAX_SLOT = 256 * 1024 * 1024


@dataclass(frozen=True)
class CodeSlot:
    offset: int
    size: int


def _name(value: bytes) -> str:
    return value.split(b"\x00", 1)[0].decode("ascii", errors="strict")


def find_code_slot(data: bytes, *, require_unsigned: bool = True) -> CodeSlot:
    if len(data) < _HEADER.size:
        raise ValueError("Mach-O header is truncated")
    magic, cpu, subtype, filetype, ncmds, sizeofcmds, flags, reserved = _HEADER.unpack_from(data)
    if magic != MAGIC_64 or cpu != ARM64 or filetype != MH_EXECUTE:
        raise ValueError("expected a thin ARM64 Mach-O executable")
    if not 1 <= ncmds <= 256 or not 0 < sizeofcmds <= 65536:
        raise ValueError("invalid Mach-O load commands")
    end = _HEADER.size + sizeofcmds
    if end > len(data):
        raise ValueError("Mach-O load commands are truncated")
    position = _HEADER.size
    matches: list[CodeSlot] = []
    for _ in range(ncmds):
        if position + 8 > end:
            raise ValueError("Mach-O load command header is truncated")
        cmd, cmdsize = struct.unpack_from("<II", data, position)
        if cmdsize < 8 or cmdsize % 8 or position + cmdsize > end:
            raise ValueError("invalid Mach-O command length")
        if cmd == LC_CODE_SIGNATURE and require_unsigned:
            raise ValueError("template executable contains a code-signature command")
        if cmd == LC_SEGMENT_64:
            if cmdsize < _SEGMENT.size:
                raise ValueError("truncated Mach-O segment")
            (
                _, _, segname, vmaddr, vmsize, fileoff, filesize,
                maxprot, initprot, nsects, segflags
            ) = _SEGMENT.unpack_from(data, position)
            if nsects > 256 or _SEGMENT.size + nsects * _SECTION.size > cmdsize:
                raise ValueError("invalid Mach-O section table")
            if fileoff > len(data) or filesize > len(data) - fileoff:
                raise ValueError("Mach-O segment extends beyond executable")
            if _name(segname) == "__MRCODE" and not (
                maxprot & VM_PROT_EXECUTE and initprot & VM_PROT_EXECUTE
            ):
                raise ValueError("AOT code segment is not executable")
            for index in range(nsects):
                section_at = position + _SEGMENT.size + index * _SECTION.size
                sect = _SECTION.unpack_from(data, section_at)
                secname, secseg, address, secsize, secoff, align, reloff, nreloc, sflags, _, _, _ = sect
                if _name(secseg) == "__MRCODE" and _name(secname) == "__text":
                    if _name(segname) != "__MRCODE":
                        raise ValueError("AOT code section is in the wrong segment")
                    if sflags & 0xFF:  # Must be S_REGULAR, not virtual zerofill.
                        raise ValueError("AOT slot is not file-backed regular text")
                    if align < 2 or secoff % 4:
                        raise ValueError("AOT code section is not aligned")
                    if not MIN_SLOT <= secsize <= MAX_SLOT:
                        raise ValueError("AOT code section size is invalid")
                    if secoff < fileoff or secsize > fileoff + filesize - secoff:
                        raise ValueError("AOT slot is outside the mapped segment")
                    if secoff < end:
                        raise ValueError("AOT slot overlaps Mach-O load commands")
                    matches.append(CodeSlot(secoff, secsize))
        position += cmdsize
    if position != end:
        raise ValueError("Mach-O command sizes do not match their header")
    if len(matches) != 1:
        raise ValueError("expected exactly one native AOT code section")
    return matches[0]


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def normalized_text_sha256(path: Path) -> str:
    """The build-time Windows C runtime emits CRLF instead of POSIX LF."""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        pending_cr = b""
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            data = pending_cr + data
            if data.endswith(b"\r"):
                data, pending_cr = data[:-1], b"\r"
            else:
                pending_cr = b""
            digest.update(data.replace(b"\r\n", b"\n"))
        if pending_cr:
            digest.update(pending_cr)
    return digest.hexdigest()


def require_zero_slot(executable: bytes, slot: CodeSlot) -> None:
    if slot.offset + slot.size > len(executable):
        raise ValueError("AOT slot exceeds the executable")
    if any(executable[slot.offset:slot.offset + slot.size]):
        raise ValueError("AOT template slot contains nonzero or game-derived bytes")
