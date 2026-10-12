#!/usr/bin/env python3
"""Assemble a standalone Windows native-IPA builder from verified public tools.

Run on a build host, never in CI with original game files. The kit is not a
distributable release until native iPhone signing and legal reviews pass.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import stat
import sys
import tempfile
import unicodedata
import zipfile
from pathlib import Path, PurePosixPath

sys.dont_write_bytecode = True

from asset_contract import OFFICIAL_ENGINE_SHA256
from common import ROOT, public_files
from native_macho import file_sha256

PYTHON_3147_EMBED_AMD64_SHA256 = (
    "d297e5ff019966817ad8502465176139f2d3d840fa4ed84b13bed399a6ab1f15"
)
PYTHON_FILES_REQUIRED = frozenset((
    "python.exe", "python314.zip", "python314._pth", "LICENSE.txt",
    "_sqlite3.pyd",
))


def _unpack_verified_python(source: Path, target: Path) -> None:
    if file_sha256(source) != PYTHON_3147_EMBED_AMD64_SHA256:
        raise ValueError("official Python embeddable ZIP SHA-256 mismatch")
    with zipfile.ZipFile(source) as archive:
        entries = archive.infolist()
        if not entries or len(entries) > 100 or sum(x.file_size for x in entries) > 125*1024*1024:
            raise ValueError("unexpected Python package layout or size")
        names: set[str] = set()
        for entry in entries:
            name = entry.filename
            parts = PurePosixPath(name).parts
            mode = entry.external_attr >> 16
            normalized = unicodedata.normalize("NFC", name).casefold()
            if (not name or len(parts) != 1 or ".." in parts or "\\" in name
                    or stat.S_IFMT(mode) not in (0, stat.S_IFREG)
                    or entry.is_dir() or normalized in names):
                raise ValueError("unsafe Python package entry")
            names.add(normalized)
        if not PYTHON_FILES_REQUIRED.issubset({x.filename for x in entries}):
            raise ValueError("missing files in Python embeddable archive")
        target.mkdir()
        for entry in entries:
            with archive.open(entry) as stream, (target / entry.filename).open("wb") as output:
                shutil.copyfileobj(stream, output, length=1024*1024)


def build_kit(python_zip: Path, compiler_exe: Path, template: Path, target: Path) -> None:
    if target.exists() or target.is_symlink():
        raise ValueError("Windows kit output must be a new directory")
    if not target.parent.is_dir():
        raise ValueError("Windows kit output parent does not exist")
    if target.resolve().is_relative_to(ROOT) and not target.resolve().is_relative_to(ROOT / "build"):
        raise ValueError("Windows kit output cannot be inside the public source tree")
    if not compiler_exe.is_file() or not template.is_file():
        raise ValueError("compiler or unsigned template is missing")
    with compiler_exe.open("rb") as stream:
        header = stream.read(512)
    if header[:2] != b"MZ":
        raise ValueError("Windows compiler is not a PE executable")
    with tempfile.TemporaryDirectory(prefix=".mr-kit-", dir=target.parent) as directory:
        staged = Path(directory) / "kit"
        staged.mkdir()
        shutil.copyfile(compiler_exe, staged / "a64-compiler.exe")
        shutil.copyfile(template, staged / "MinionRush-native-template-unsigned.zip")
        _unpack_verified_python(python_zip, staged / "python")
        # The official embedded interpreter deliberately excludes the script
        # directory from sys.path. Whitelist only our source-controlled tools,
        # keep isolated mode, and never enable site or PYTHONPATH injection.
        (staged / "python/python314._pth").write_bytes(
            b"python314.zip\n.\n..\\runtime\\tools\n"
        )
        runtime = staged / "runtime"
        runtime.mkdir()
        runtime = runtime.resolve()  # macOS /var is a symlink to /private/var
        for path in public_files(ROOT):
            dest = runtime / path.relative_to(ROOT)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, dest)
        # The primary entry point and PowerShell runner are source-controlled,
        # not forked per release.
        for name in ("run-native-ipa.cmd", "run-native-ipa.ps1"):
            data = (ROOT / "tools" / name).read_bytes()
            if name.endswith(".cmd"):
                data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            (staged / name).write_bytes(data)
        runtime_hashes = {
            path.relative_to(runtime).as_posix(): file_sha256(path)
            for path in public_files(runtime)
        }
        (staged / "runtime-file-hashes.json").write_text(
            json.dumps(runtime_hashes, sort_keys=True, indent=2) + "\n",
            encoding="utf-8"
        )
        python_hashes = {
            path.name: file_sha256(path)
            for path in sorted((staged / "python").iterdir())
            if path.is_file() and not path.is_symlink()
        }
        (staged / "python-file-hashes.json").write_text(
            json.dumps(python_hashes, sort_keys=True, indent=2) + "\n",
            encoding="utf-8"
        )
        config = {
            "version": 1,
            "engine_sha256": OFFICIAL_ENGINE_SHA256,
            "template_sha256": file_sha256(staged / "MinionRush-native-template-unsigned.zip"),
            "compiler_sha256": file_sha256(staged / "a64-compiler.exe"),
            "runtime_manifest_sha256": file_sha256(runtime / "config/source_manifest.txt"),
            "runtime_files_sha256": file_sha256(staged / "runtime-file-hashes.json"),
            "python_zip_sha256": file_sha256(staged / "python/python314.zip"),
            "python_files_sha256": file_sha256(staged / "python-file-hashes.json"),
        }
        (staged / "kit-manifest.json").write_text(
            json.dumps(config, sort_keys=True, indent=2) + "\n", encoding="utf-8"
        )
        staged.rename(target)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python-embed-zip", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    try:
        build_kit(options.python_embed_zip, options.compiler, options.template, options.output)
    except (ValueError, OSError, zipfile.BadZipFile) as exc:
        print("Windows kit assembly failed:", exc, file=sys.stderr)
        return 1
    print("Portable Windows kit generated:", options.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
