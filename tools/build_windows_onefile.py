#!/usr/bin/env python3
"""Package a verified portable Windows kit as one self-extracting EXE."""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

from build_windows_kit import validate_gui_executable
from package_windows_release import package_windows_candidate
from windows_native_package import validate_kit

BUNDLE_MAGIC = b"MR1FILE!"
FOOTER = struct.Struct("<8sQQ32s")
MAX_BUNDLE_BYTES = 256 * 1024 * 1024


def build_onefile(launcher: Path, kit: Path, output: Path, *,
                  allow_test_profile: bool = False) -> str:
    if output.suffix.lower() != ".exe" or output.exists() or output.is_symlink():
        raise ValueError("output must be a new .exe")
    if not output.is_absolute() or not output.parent.is_dir():
        raise ValueError("one-file output requires an existing absolute parent")
    if launcher.is_symlink() or not launcher.is_file():
        raise ValueError("launcher must be a regular PE32+ executable")
    validate_gui_executable(launcher)
    config = validate_kit(kit)
    if config["mode"] != "release" and not allow_test_profile:
        raise ValueError("UTM test kit requires explicit --allow-test-profile")
    with tempfile.TemporaryDirectory(prefix="mr-onefile-") as temporary:
        archive = Path(temporary) / "kit.zip"
        package_windows_candidate(kit, archive, allow_test_profile=allow_test_profile)
        if archive.stat().st_size > MAX_BUNDLE_BYTES:
            raise ValueError("embedded kit exceeds size limit")
        stage = output.parent / (output.name + ".partial")
        if stage.exists() or stage.is_symlink():
            raise ValueError("temporary one-file output already exists")
        try:
            digest = hashlib.sha256()
            with stage.open("xb") as destination:
                with launcher.open("rb") as source:
                    for block in iter(lambda: source.read(1024 * 1024), b""):
                        destination.write(block)
                archive_offset = destination.tell()
                length = 0
                with archive.open("rb") as source:
                    for block in iter(lambda: source.read(1024 * 1024), b""):
                        digest.update(block)
                        destination.write(block)
                        length += len(block)
                destination.write(FOOTER.pack(BUNDLE_MAGIC, archive_offset,
                                              length, digest.digest()))
                destination.flush()
                os.fsync(destination.fileno())
            with stage.open("rb") as verify:
                verify.seek(-FOOTER.size, 2)
                magic, offset, size, expected = FOOTER.unpack(verify.read(FOOTER.size))
                if (magic != BUNDLE_MAGIC or size != length or offset != archive_offset
                        or expected != digest.digest()):
                    raise ValueError("one-file trailer verification failed")
            stage.replace(output)
        finally:
            stage.unlink(missing_ok=True)
    with output.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--launcher", type=Path, required=True)
    parser.add_argument("--kit", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--allow-test-profile", action="store_true")
    args = parser.parse_args()
    try:
        digest = build_onefile(args.launcher, args.kit, args.output,
                               allow_test_profile=args.allow_test_profile)
    except (ValueError, OSError) as exc:
        print("Windows one-file build failed:", exc, file=sys.stderr)
        return 1
    print("Single Windows executable:", args.output)
    print("SHA-256:", digest)
    print("Do not publish private game or translation content without rights clearance.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
