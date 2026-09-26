#!/usr/bin/env python3
"""Validate the public installer bundle and its downloadable archive."""

from __future__ import annotations

import argparse
import plistlib
import re
import stat
import struct
import sys
import zipfile
from pathlib import Path

sys.dont_write_bytecode = True

from common import ROOT, public_files


def bundle_files(root: Path, app: Path) -> dict[str, Path]:
    root = root.resolve()
    if app.is_symlink() or not app.is_dir():
        raise ValueError("The installer must be an ordinary app directory.")
    files = {}
    directories = set()
    for path in app.rglob("*"):
        name = path.relative_to(app).as_posix()
        if path.is_symlink():
            raise ValueError("Links are not allowed in the installer bundle.")
        if path.is_file():
            files[name] = path
        elif path.is_dir():
            directories.add(name)
        else:
            raise ValueError(f"Unexpected installer entry: {name}")
    source = {"Contents/Info.plist": root / "installer/Info.plist",
              "Contents/Resources/installer_ui.json": root / "config/installer_ui.json"}
    source.update({"Contents/Resources/Runtime/" + path.relative_to(root).as_posix(): path
                   for path in public_files(root)})
    generated = {"Contents/MacOS/MinionRushInstaller", "Contents/Resources/Installer.icns",
                 "Contents/_CodeSignature/CodeResources"}
    if set(files) != set(source) | generated:
        raise ValueError("Installer files do not match the public bundle contract.")
    expected_directories = {parent.as_posix() for name in files
                            for parent in Path(name).parents if parent != Path(".")}
    if directories != expected_directories:
        raise ValueError("Installer directories do not match the public bundle contract.")
    for name, original in source.items():
        if files[name].read_bytes() != original.read_bytes():
            raise ValueError(f"Bundled public source differs: {name}")
    info = plistlib.loads(files["Contents/Info.plist"].read_bytes())
    if (not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9 -]*", info["CFBundleName"])
            or app.name != info["CFBundleName"] + ".app"
            or not re.fullmatch(r"\d+\.\d+\.\d+", info["CFBundleShortVersionString"])):
        raise ValueError("Invalid installer name or release version.")
    executable = files["Contents/MacOS/MinionRushInstaller"]
    with executable.open("rb") as binary:
        header = binary.read(8)
    if header != struct.pack("<II", 0xFEEDFACF, 0x0100000C):
        raise ValueError("Installer executable must be a 64-bit ARM Mach-O file.")
    if not executable.stat().st_mode & 0o111:
        raise ValueError("Installer executable permissions are missing.")
    if files["Contents/Resources/Installer.icns"].read_bytes()[:4] != b"icns":
        raise ValueError("Installer icon is not an ICNS file.")
    return files


def validate_archive(root: Path, app: Path, archive: Path) -> None:
    files = bundle_files(root, app)
    folder = app.stem
    expected = {f"{folder}/{app.name}/{name}": path for name, path in files.items()}
    expected.update({f"{folder}/README.md": root / "docs/INSTALLER_DOWNLOAD.md",
                     f"{folder}/LICENSE": root / "LICENSE"})
    directories = {parent.as_posix() + "/" for name in expected
                   for parent in Path(name).parents if parent != Path(".")}
    seen: set[str] = set()
    with zipfile.ZipFile(archive) as release:
        for member in release.infolist():
            name = member.filename
            if name in seen or stat.S_ISLNK(member.external_attr >> 16):
                raise ValueError(f"Duplicate or linked archive entry: {name}")
            seen.add(name)
            if member.is_dir():
                if name not in directories or member.file_size:
                    raise ValueError(f"Unexpected archive directory: {name}")
                continue
            if name not in expected:
                raise ValueError(f"Unexpected archive file: {name}")
            path = expected[name]
            if member.file_size != path.stat().st_size or release.read(member) != path.read_bytes():
                raise ValueError(f"Archive content differs: {name}")
            if name.endswith("/Contents/MacOS/MinionRushInstaller"):
                if not (member.external_attr >> 16) & 0o111:
                    raise ValueError("Archive lost executable permissions.")
    if set(expected) != seen - directories:
        raise ValueError("Archive is missing required files.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args()
    try:
        files = bundle_files(args.root, args.app)
        if args.archive:
            validate_archive(args.root, args.app, args.archive)
    except (OSError, ValueError, KeyError, plistlib.InvalidFileException, zipfile.BadZipFile) as exc:
        print(f"Installer validation failed: {exc}", file=sys.stderr)
        return 1
    print(f"Installer validation passed ({len(files)} public bundle files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
