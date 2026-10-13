#!/usr/bin/env python3
"""Prepare a private unsigned IPA from the Mac native Builder kit."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import tempfile
from pathlib import Path

from native_ipa import assemble_unsigned_ipa
from native_ipa_output import assemble_output
from prepare_native_aot import prepare


def _sha(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def validate_native_kit(directory: Path) -> dict:
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError("private Mac native kit is missing")
    manifest = directory / "manifest.json"
    if manifest.is_symlink() or not manifest.is_file():
        raise ValueError("private Mac native kit manifest is missing")
    config = json.loads(manifest.read_text(encoding="utf-8"))
    if not isinstance(config, dict) or set(config) != {
        "version", "template_sha256", "compiler_sha256", "hungarian_sha256",
    } or config["version"] != 1:
        raise ValueError("unsupported private Mac native kit")
    expected = {"manifest.json", "template.zip", "a64-compiler"}
    if config["hungarian_sha256"] is not None:
        expected.add("hu.xlf")
    if {part.name for part in directory.iterdir()} != expected:
        raise ValueError("unexpected files in the private Mac kit")
    for name, key in (("template.zip", "template_sha256"),
                      ("a64-compiler", "compiler_sha256"),
                      ("hu.xlf", "hungarian_sha256")):
        digest = config[key]
        if digest is None and name == "hu.xlf":
            continue
        if (not isinstance(digest, str) or len(digest) != 64 or
                any(char not in "0123456789abcdef" for char in digest)):
            raise ValueError("invalid private kit checksum")
        path = directory / name
        if not path.is_file() or path.is_symlink() or _sha(path) != digest:
            raise ValueError("private Mac kit file checksum mismatch: " + name)
    compiler = directory / "a64-compiler"
    if not os.access(compiler, os.X_OK):
        raise ValueError("the private Mac AOT compiler is not executable")
    return config


def build_unsigned(source: Path, destination: Path, kit: Path, *,
                   hungarian: bool = True, overwrite: bool = False) -> None:
    config = validate_native_kit(kit)
    if not source.is_file() or source.is_symlink() or source.suffix.lower() != ".zip":
        raise ValueError("choose an original local 1.8.1g ZIP")
    if hungarian and config["hungarian_sha256"] is None:
        raise ValueError("Hungarian is not included in this Mac Builder")
    with tempfile.TemporaryDirectory(prefix="minion-mac-private-") as scratch:
        private = Path(scratch).resolve() / "private-aot"
        print("1/3: Validating original ZIP and preparing native ARM64 blocks", flush=True)
        prepare(source, private, prebuilt_compiler=kit / "a64-compiler",
                compiler_sha256=config["compiler_sha256"],
                translations_root=kit if hungarian else None,
                enable_localizations=hungarian)
        print("2/3: Assembling unsigned IPA and original game data", flush=True)
        assemble_output(
            destination,
            lambda path: assemble_unsigned_ipa(kit / "template.zip", private, path),
            overwrite=overwrite,
        )
    print("3/3: Native unsigned IPA assembled and verified", flush=True)
    print("File:", destination, flush=True)
