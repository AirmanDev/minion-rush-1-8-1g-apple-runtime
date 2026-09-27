"""Atomic packaging of a signed application as an IPA."""

from __future__ import annotations

import os
import tempfile
import zipfile
from pathlib import Path


def export_ipa(application: Path, destination: Path) -> None:
    if destination.suffix.casefold() != ".ipa":
        raise ValueError("The exported archive must use the .ipa extension.")
    if not application.is_dir() or application.is_symlink() or application.suffix != ".app":
        raise ValueError("The signed application bundle is missing.")
    descriptor, temporary = tempfile.mkstemp(prefix=".minion-rush-", suffix=".ipa", dir=destination.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
                for source in sorted(application.rglob("*")):
                    if source.is_symlink():
                        raise ValueError("Linked files cannot be exported in the application.")
                    if source.is_file():
                        archive.write(source, "Payload/" + application.name + "/"
                                      + source.relative_to(application).as_posix())
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, destination)
    finally:
        Path(temporary).unlink(missing_ok=True)
