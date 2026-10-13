#!/usr/bin/env python3
"""Assemble a standalone Windows native-IPA builder from verified public tools.

Run on a build host, never in CI with original game files. The kit is not a
distributable release until native iPhone signing and legal reviews pass.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
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


def validate_gui_executable(source: Path) -> None:
    """Require a 64-bit Windows graphical PE image, never an arbitrary file."""
    with source.open("rb") as file:
        dos = file.read(64)
        if len(dos) != 64 or dos[:2] != b"MZ":
            raise ValueError("Windows GUI does not have a valid DOS header")
        offset = struct.unpack_from("<I", dos, 60)[0]
        if not 64 <= offset < 4096:
            raise ValueError("invalid Windows GUI PE offset")
        file.seek(offset)
        pe = file.read(128)
        if len(pe) != 128 or pe[:4] != b"PE\0\0":
            raise ValueError("invalid Windows GUI PE signature")
        machine = struct.unpack_from("<H", pe, 4)[0]
        optional_header_size = struct.unpack_from("<H", pe, 20)[0]
        if machine != 0x8664 or optional_header_size < 72:
            raise ValueError("GUI must target Windows x64")
        if struct.unpack_from("<H", pe, 24)[0] != 0x20B:
            raise ValueError("GUI executable must be PE32+")
        if struct.unpack_from("<H", pe, 24+68)[0] != 2:
            raise ValueError("GUI executable must be a Windows graphical app")


def build_kit(python_zip: Path, compiler_exe: Path, template: Path,
              gui_exe: Path, target: Path, *, mode: str = "release",
              hungarian_xlf: Path | None = None) -> None:
    if mode not in ("release", "utm-test"):
        raise ValueError("invalid Windows kit mode")
    marker = "UTM test build".encode("utf-16le")
    if (marker in gui_exe.read_bytes()) != (mode == "utm-test"):
        raise ValueError("GUI executable does not match selected Windows kit mode")
    if target.exists() or target.is_symlink():
        raise ValueError("Windows kit output must be a new directory")
    if not target.parent.is_dir():
        raise ValueError("Windows kit output parent does not exist")
    if target.resolve().is_relative_to(ROOT) and not target.resolve().is_relative_to(ROOT / "build"):
        raise ValueError("Windows kit output cannot be inside the public source tree")
    if not compiler_exe.is_file() or not template.is_file() or not gui_exe.is_file():
        raise ValueError("compiler, GUI or unsigned template is missing")
    validate_gui_executable(gui_exe)
    with compiler_exe.open("rb") as stream:
        header = stream.read(512)
    if header[:2] != b"MZ":
        raise ValueError("Windows compiler is not a PE executable")
    with tempfile.TemporaryDirectory(prefix=".mr-kit-", dir=target.parent) as directory:
        staged = Path(directory) / "kit"
        staged.mkdir()
        shutil.copyfile(compiler_exe, staged / "a64-compiler.exe")
        shutil.copyfile(gui_exe, staged / "MinionRush Builder.exe")
        shutil.copyfile(template, staged / "MinionRush-native-template-unsigned.zip")
        if hungarian_xlf is not None:
            if (hungarian_xlf.name.lower() != "hu.xlf" or hungarian_xlf.is_symlink()
                    or not hungarian_xlf.is_file() or hungarian_xlf.stat().st_size > 2 * 1024 * 1024):
                raise ValueError("private Hungarian catalog must be a regular hu.xlf under 2 MB")
            languages = staged / "private-localizations"
            languages.mkdir()
            shutil.copyfile(hungarian_xlf, languages / "hu.xlf")
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
            relative = path.relative_to(ROOT)
            if mode == "release" and relative.as_posix() == "tools/windows_utm_transport.py":
                continue
            dest = runtime / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, dest)
        if mode == "release":
            manifest = runtime / "config/source_manifest.txt"
            names = [name for name in manifest.read_text(encoding="utf-8").splitlines()
                     if name != "tools/windows_utm_transport.py"]
            manifest.write_text("\n".join(names) + "\n", encoding="utf-8")
        (staged / "README-WINDOWS.txt").write_text(
            "Minion Rush - native IPA builder\n"
            "================================\n"
            "Open MinionRush Builder.exe. Drag your original 1.8.1g ZIP onto the\n"
            "window or choose it with Browse. Select a new .ipa save path.\n"
            + ("Optional local Hungarian game-language catalog was supplied.\n"
               if hungarian_xlf is not None else
               "No third-party game translation ships in this Builder.\n")
            + "The Builder interface supports English and Hungarian.\n"
            + "Original game languages come from your source ZIP.\n"
            + ("UTM test profile: supports private SPICE shared folders.\n"
               if mode == "utm-test" else "Release profile: no UTM-specific support.\n") +
            "The output is unsigned. Sign it with Sideloadly before installing.\n"
            "The original game ZIP and final IPA are private. Do not publish them.\n"
            "The generic iOS template contains no original game data, precomputed\n"
            "engine bindings or translated machine instructions.\n"
            "The application is not an Apple product or official game release.\n",
            encoding="ascii",
        )
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
            "version": 5,
            "mode": mode,
            "hungarian_xlf_sha256": (
                file_sha256(staged / "private-localizations/hu.xlf") if hungarian_xlf is not None
                else None
            ),
            "engine_sha256": OFFICIAL_ENGINE_SHA256,
            "template_sha256": file_sha256(staged / "MinionRush-native-template-unsigned.zip"),
            "compiler_sha256": file_sha256(staged / "a64-compiler.exe"),
            "gui_sha256": file_sha256(staged / "MinionRush Builder.exe"),
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
    parser.add_argument("--gui-exe", type=Path, required=True)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("release", "utm-test"), default="release")
    parser.add_argument("--hungarian-xlf", type=Path,
                        help="private language catalog for a one-file or private kit build")
    options = parser.parse_args()
    try:
        build_kit(options.python_embed_zip, options.compiler, options.template,
                  options.gui_exe, options.output, mode=options.mode,
                  hungarian_xlf=options.hungarian_xlf)
    except (ValueError, OSError, zipfile.BadZipFile) as exc:
        print("Windows kit assembly failed:", exc, file=sys.stderr)
        return 1
    print("Portable Windows kit generated:", options.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
