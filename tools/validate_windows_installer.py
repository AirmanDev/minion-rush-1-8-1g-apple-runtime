"""Check a staged Windows package before creating its download archive."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def validate(root: Path, package: Path, machine: int = 0x8664) -> None:
    expected = {"MinionRushInstaller.exe", "installer_ui.json", "LICENSE", "README.md",
                "THIRD_PARTY_NOTICES.txt", "DOTNET_LICENSE.txt", "DOTNET_NOTICES.txt", "backend"}
    actual = {entry.name for entry in package.iterdir()}
    if actual != expected:
        raise ValueError(f"Unexpected Windows package entries: {sorted(actual ^ expected)}")
    for name, source in (("installer_ui.json", "config/installer_ui.json"),
                         ("LICENSE", "LICENSE"), ("README.md", "docs/INSTALLER_WINDOWS.md")):
        if (package / name).read_bytes() != (root / source).read_bytes():
            raise ValueError(f"Package source differs: {name}")
    for entry in package.rglob("*"):
        if entry.is_symlink():
            raise ValueError("Linked files cannot be distributed.")
        if entry.suffix.casefold() in {".ipa", ".apk", ".mobileprovision", ".p12", ".pfx", ".key", ".so"}:
            raise ValueError(f"Private or non-Windows file in package: {entry.name}")
    for executable in (package / "MinionRushInstaller.exe", package / "backend/MinionRushDeviceBackend.exe"):
        with executable.open("rb") as binary:
            if binary.read(2) != b"MZ":
                raise ValueError(f"Not a Windows executable: {executable.name}")
            binary.seek(0x3C)
            offset = struct.unpack("<I", binary.read(4))[0]
            binary.seek(offset)
            if binary.read(4) != b"PE\x00\x00" or struct.unpack("<H", binary.read(2))[0] != machine:
                raise ValueError(f"Incorrect Windows executable architecture: {executable.name}")
    for name in ("THIRD_PARTY_NOTICES.txt", "DOTNET_LICENSE.txt", "DOTNET_NOTICES.txt"):
        if not (package / name).read_text(encoding="utf-8-sig").strip():
            raise ValueError(f"Missing license notices: {name}")
    json.loads((package / "installer_ui.json").read_text())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--architecture", choices=["x64", "arm64"], default="x64")
    args = parser.parse_args()
    validate(Path(__file__).resolve().parents[1], args.package,
             0x8664 if args.architecture == "x64" else 0xAA64)
    print("Windows package validation passed")
