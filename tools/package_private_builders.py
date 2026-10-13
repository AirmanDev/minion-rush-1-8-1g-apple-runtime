#!/usr/bin/env python3
"""Package local, fully portable Mac/Windows Builders for private testing.

This tool deliberately has no GitHub upload or public publishing action.
The kit's generated iOS template and bundled translation need a separate
rights review before any redistribution.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

from build_windows_onefile import BUNDLE_MAGIC, FOOTER, MAX_BUNDLE_BYTES
from common import ROOT
from native_macos_package import validate_native_kit
from native_macho import file_sha256

APP = "Minion Rush IPA Builder.app"
WIN = "MinionRush-Builder-Windows-x64.exe"
MAC = "MinionRush-Builder-macOS-arm64.zip"
RELEASE_DIR = ROOT / "build/private-releases"


def verify_windows(executable: Path) -> None:
    if not executable.is_file() or executable.is_symlink():
        raise ValueError("missing regular Windows portable executable")
    with executable.open("rb") as stream:
        size = stream.seek(0, os.SEEK_END)
        if size <= FOOTER.size:
            raise ValueError("Windows executable is too small")
        stream.seek(-FOOTER.size, os.SEEK_END)
        magic, offset, length, expected = FOOTER.unpack(stream.read(FOOTER.size))
        if (magic != BUNDLE_MAGIC or not 0 < length <= MAX_BUNDLE_BYTES
                or offset < 64 or offset + length + FOOTER.size != size):
            raise ValueError("invalid Windows one-file footer")
        stream.seek(offset)
        payload = stream.read(length)
    if hashlib.sha256(payload).digest() != expected:
        raise ValueError("Windows kit checksum mismatch")
    with zipfile.ZipFile(io.BytesIO(payload)) as kit:
        if kit.testzip():
            raise ValueError("corrupt embedded Windows kit")
        names = set(kit.namelist())
        if not any(name.endswith("/private-localizations/hu.xlf") for name in names):
            raise ValueError("Windows Builder is missing private Hungarian selection")
        if not any(name.endswith("/MinionRush-native-template-unsigned.zip")
                   for name in names):
            raise ValueError("Windows Builder is missing unsigned iOS template")
        if any(name.lower().endswith((".ipa", ".apk", ".jpk")) for name in names):
            raise ValueError("original game content must not enter the Builder")


def verify_mac(app: Path, version: str) -> None:
    if app.is_symlink() or not app.is_dir() or app.suffix != ".app":
        raise ValueError("invalid Mac Builder")
    resources = app / "Contents/Resources"
    config = validate_native_kit(resources / "NativeKit")
    if config["hungarian_sha256"] is None:
        raise ValueError("Mac Builder is missing private Hungarian selection")
    if not (resources / "PythonRuntime/bin/python3.14").is_file():
        raise ValueError("Mac Builder has no embedded Python")
    info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
    if info.get("CFBundleShortVersionString") != version:
        raise ValueError("Mac Builder version does not match release")
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)],
                   check=True, timeout=45)


def package(app: Path, windows: Path, output: Path, version: str) -> None:
    if not version or any(c not in "0123456789." for c in version):
        raise ValueError("expected a numeric Mac release version")
    if output.exists() or output.is_symlink():
        raise ValueError("private release directory already exists")
    if (not output.is_absolute() or not output.parent.is_dir()
            or output.parent.resolve() != RELEASE_DIR.resolve()):
        raise ValueError("private releases must stay under build/private-releases")
    verify_mac(app, version)
    verify_windows(windows)
    with tempfile.TemporaryDirectory(prefix=".mr-private-release-", dir=output.parent) as tmp:
        stage = Path(tmp) / "files"
        stage.mkdir()
        normalized = Path(tmp) / APP
        shutil.copytree(app, normalized, symlinks=False)
        verify_mac(normalized, version)
        archive = stage / MAC
        subprocess.run(["ditto", "-c", "-k", "--keepParent", "--norsrc",
                        str(normalized), str(archive)], check=True, timeout=95)
        with zipfile.ZipFile(archive) as zipped:
            if zipped.testzip() is not None:
                raise ValueError("corrupt private Mac Builder ZIP")
            names = zipped.namelist()
            if not any(name.endswith("/NativeKit/template.zip") for name in names):
                raise ValueError("Mac ZIP lost its iOS template")
            if not any(name.endswith("/NativeKit/hu.xlf") for name in names):
                raise ValueError("Mac ZIP lost the Hungarian catalog")
            if any(name.lower().endswith((".ipa", ".apk", ".jpk")) for name in names):
                raise ValueError("Mac ZIP contains game content")
        extracted = Path(tmp) / "extract"
        extracted.mkdir()
        subprocess.run(["ditto", "-x", "-k", str(archive), str(extracted)],
                       check=True, timeout=95)
        verify_mac(extracted / APP, version)
        shutil.copy2(windows, stage / WIN)
        verify_windows(stage / WIN)
        hashes = "".join(f"{file_sha256(stage / name)}  {name}\n"
                         for name in (MAC, WIN))
        (stage / "SHA256SUMS.txt").write_text(hashes, encoding="ascii")
        stage.rename(output)
    for line in (output / "SHA256SUMS.txt").read_text().splitlines():
        digest, filename = line.split("  ", 1)
        if file_sha256(output / filename) != digest:
            raise ValueError("private release checksum mismatch")
    print("Validated private Mac and Windows Builders:", output)
    print("No complete game IPA or original game ZIP bundled.")
    print("Not cleared for public download or redistribution.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--windows-exe", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        package(args.app, args.windows_exe, args.output, args.version)
    except (OSError, ValueError, subprocess.SubprocessError, zipfile.BadZipFile) as e:
        print(f"Private Builder package rejected: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
