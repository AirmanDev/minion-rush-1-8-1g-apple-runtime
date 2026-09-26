#!/usr/bin/env python3
"""Shared file, source-manifest, and JSON primitives for project tools."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Iterator

ROOT = Path(__file__).resolve().parent.parent


def public_files(source: Path) -> list[Path]:
    source = source.resolve()
    names = (source / "config/source_manifest.txt").read_text().splitlines()
    files: list[Path] = []
    for name in names:
        relative = Path(name)
        if not name or relative.is_absolute() or ".." in relative.parts:
            raise ValueError("Invalid public source manifest.")
        path = source / relative
        if path.is_symlink() or not path.is_file() or source not in path.resolve().parents:
            raise ValueError(f"Missing or unsafe public source file: {name}")
        files.append(path)
    return files


# Metadata that Finder, Android and SQLite may recreate. Validators report it
# but never modify the inspected tree.
DISPOSABLE_NAMES = frozenset({".DS_Store", ".nomedia"})
DISPOSABLE_SUFFIXES = (".updated", "-wal", "-shm")
DISPOSABLE_PREFIX = "._"

# The engine writes latin-1, the DLC layer utf-8, and neither declares an
# encoding, so both are tried in a fixed order.
JSON_ENCODINGS = ("utf-8", "latin-1")


def is_disposable(name: str) -> bool:
    return (
        name in DISPOSABLE_NAMES
        or name.startswith(DISPOSABLE_PREFIX)
        or name.endswith(DISPOSABLE_SUFFIXES)
    )


def walk_files(tree: Path, ignored: frozenset[str] = frozenset()) -> Iterator[Path]:
    """Every file under `tree` in a stable order, never entering `ignored`.

    Pruning happens during the walk, so the asset and build trees cost
    nothing when a tool only cares about the source tree.
    """
    for parent, directories, names in os.walk(tree):
        directories[:] = sorted(name for name in directories if name not in ignored)
        base = Path(parent)
        for name in sorted(names):
            yield base / name


def content_files(tree: Path, ignored: frozenset[str] = frozenset()) -> Iterator[Path]:
    """Every file under `tree` that is content rather than metadata."""
    for path in walk_files(tree, ignored):
        if not is_disposable(path.name):
            yield path


def load_json(path: Path, encoding: str | None = None) -> Any:
    """Parse a JSON file, trying utf-8 then latin-1 unless one is named."""
    data = path.read_bytes()
    for candidate in (encoding,) if encoding else JSON_ENCODINGS:
        try:
            return json.loads(data.decode(candidate))
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
    raise ValueError(f"invalid JSON: {path}")


def load_json_object(path: Path, encoding: str | None = None) -> dict:
    value = load_json(path, encoding)
    if not isinstance(value, dict):
        raise ValueError(f"not a JSON object: {path}")
    return value
