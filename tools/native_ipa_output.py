#!/usr/bin/env python3
"""Commit a fully assembled unsigned IPA without damaging an existing output."""

from __future__ import annotations

import os
import tempfile
from pathlib import Path
from typing import Callable


def assemble_output(destination: Path, assemble: Callable[[Path], None], *,
                    overwrite: bool = False) -> None:
    if not destination.is_absolute() or destination.suffix.lower() != ".ipa":
        raise ValueError("destination must be an absolute .ipa path")
    if not destination.parent.is_dir() or destination.parent.is_symlink():
        raise ValueError("destination parent must be a regular directory")
    if destination.is_symlink():
        raise ValueError("linked output is not allowed")
    existing = destination.exists()
    if existing and (not overwrite or not destination.is_file()):
        raise ValueError("existing IPA requires an explicit overwrite confirmation")
    if overwrite and not existing:
        raise ValueError("approved output has disappeared")
    old = destination.stat() if existing else None
    with tempfile.TemporaryDirectory(prefix=".mr-ipa-", dir=destination.parent) as temporary:
        candidate = Path(temporary) / destination.name
        assemble(candidate)
        if not candidate.is_file() or candidate.is_symlink() or not candidate.stat().st_size:
            raise ValueError("IPA assembly did not produce an output")
        if old is not None:
            if destination.is_symlink() or not destination.is_file():
                raise ValueError("destination changed during build")
            now = destination.stat()
            if (now.st_dev, now.st_ino, now.st_size, now.st_mtime_ns) != (
                    old.st_dev, old.st_ino, old.st_size, old.st_mtime_ns):
                raise ValueError("destination changed during build")
        elif destination.exists() or destination.is_symlink():
            raise ValueError("destination was created during build")
        os.replace(candidate, destination)
