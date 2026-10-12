#!/usr/bin/env python3
"""Create a private Windows GUI release candidate with a reproducible inventory.

This command never publishes artifacts. A legal review is required before any
public release of a template containing source-derived engine metadata.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
import tempfile
import zipfile
from pathlib import Path

sys.dont_write_bytecode = True

from windows_native_package import validate_kit


def package_windows_candidate(kit: Path, output: Path) -> str:
    if output.suffix.lower() != ".zip" or output.exists() or output.is_symlink():
        raise ValueError("choose a new .zip candidate path")
    if not output.is_absolute() or not output.parent.is_dir():
        raise ValueError("output path must be absolute with an existing parent")
    kit = kit.resolve(strict=True)
    validate_kit(kit)
    files = sorted(path for path in kit.rglob("*") if path.is_file())
    if not files or len(files) > 350:
        raise ValueError("unexpected number of portable runtime files")
    if any(path.is_symlink() for path in kit.rglob("*")):
        raise ValueError("symbolic links are not allowed in the portable kit")
    names = [path.relative_to(kit).as_posix() for path in files]
    if any(name.endswith((".ipa", ".xlf", ".apk", ".so"))
           or name.lower().startswith("assets/") for name in names):
        raise ValueError("private game or translation files are present in the release kit")
    descriptor, temporary = tempfile.mkstemp(prefix=".minion-windows-", suffix=".zip",
                                             dir=output.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED,
                                 compresslevel=7, allowZip64=True) as archive:
                for path in files:
                    entry = zipfile.ZipInfo(
                        "MinionRush-Portable-Windows/" + path.relative_to(kit).as_posix(),
                        date_time=(2026, 1, 1, 0, 0, 0),
                    )
                    entry.compress_type = zipfile.ZIP_DEFLATED
                    entry.create_system = 3
                    entry.external_attr = (0o100755 if path.suffix.lower() == ".exe"
                                           else 0o100644) << 16
                    archive.writestr(entry, path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED,
                                     compresslevel=7)
            stream.flush()
            os.fsync(stream.fileno())
        with zipfile.ZipFile(temporary) as archive:
            assert archive.testzip() is None
            if len(archive.namelist()) != len(files):
                raise ValueError("release archive has an incomplete file inventory")
        os.replace(temporary, output)
    finally:
        Path(temporary).unlink(missing_ok=True)
    digest = hashlib.sha256()
    with output.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kit", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        digest = package_windows_candidate(args.kit, args.output)
    except (ValueError, OSError, zipfile.BadZipFile) as exc:
        print("Windows release candidate error:", exc, file=sys.stderr)
        return 1
    print("Private Windows release candidate:", args.output)
    print("SHA-256:", digest)
    print("Do not publish without legal review of the unsigned iOS template.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
