#!/usr/bin/env python3
"""Prepare private native ARM64 blocks from a user-owned Minion Rush 1.8.1g ZIP.

This is a cross-platform AOT preparation step, NOT an IPA producer. The output
contains proprietary game-derived code and game assets; never distribute it.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

sys.dont_write_bytecode = True

from common import ROOT
from install_assets import install_release
from localize import prepare as prepare_localizations

AOT_PRODUCTS = ("minion_code.bin", "minion_map.inc", "rejected-candidates.tsv",
                "game_bindings.h")
COMPILER_SOURCES = ("src/native/a64_compiler.c", "src/native/elf_loader.c",
                    "src/native/elf_loader.h", "src/native/host_pages.h")


def compile_host_compiler(destination: Path, cc: str = "clang") -> None:
    """Produce a host-native AOT translator; does not use any game inputs."""
    if destination.exists() or destination.is_symlink():
        raise ValueError("refusing to overwrite an existing compiler")
    compiler = shutil.which(cc)
    if compiler is None:
        raise ValueError(f"C compiler not found: {cc}")
    cmd = [
        compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-DMR_ELF_BUILD_TOOLS",
        "-I", str(ROOT / "src/native"),
        "-o", str(destination),
        str(ROOT / "src/native/a64_compiler.c"),
        str(ROOT / "src/native/elf_loader.c"),
    ]
    if os.name == "nt":
        cmd += ["-D_CRT_SECURE_NO_WARNINGS"]
    if sys.platform == "darwin":
        cmd += ["-D_DARWIN_C_SOURCE"]
    elif sys.platform.startswith("linux"):
        cmd += ["-D_DEFAULT_SOURCE"]
    subprocess.run(cmd, check=True, timeout=120)


def check_destination(destination: Path) -> Path:
    """No overwrites, symlink traversal, or accidental public-source output."""
    if not destination.is_absolute():
        raise ValueError("output must be an absolute directory path")
    if destination.exists() or destination.is_symlink():
        raise ValueError("output must be a new, non-existing directory")
    parent = destination.parent
    if not parent.is_dir() or parent.is_symlink() or parent.resolve() != parent:
        raise ValueError("output parent must be an existing, real directory")
    # Under the source repo, only the ignored build/ tree is private.
    if destination.is_relative_to(ROOT) and not destination.is_relative_to(ROOT / "build"):
        raise ValueError("private output must be outside the repo or inside build/")
    if destination == parent or parent == Path(parent.anchor):
        raise ValueError("unsafe destination directory")
    return destination


def prepare(zip_path: Path, destination: Path, *, cc: str = "clang",
            prebuilt_compiler: Path | None = None,
            compiler_sha256: str | None = None,
            translations_root: Path | None = None) -> dict:
    zip_path = zip_path.resolve(strict=True)
    if not zip_path.is_file() or zip_path.suffix.lower() != ".zip":
        raise ValueError("source must be a local ZIP archive")
    check_destination(destination)
    with tempfile.TemporaryDirectory(prefix=".mr-private-", dir=destination.parent) as directory:
        staging = Path(directory)
        assets = staging / "assets"
        owned_compiler = prebuilt_compiler is None
        if owned_compiler:
            if compiler_sha256 is not None:
                raise ValueError("unexpected compiler SHA-256 without a prebuilt compiler")
            compiler = staging / ("a64-compiler.exe" if os.name == "nt" else "a64-compiler")
            print("Building host-native ARM32-to-ARM64 translator", flush=True)
            compile_host_compiler(compiler, cc=cc)
        else:
            if (not compiler_sha256 or len(compiler_sha256) != 64
                    or any(char not in "0123456789abcdef" for char in compiler_sha256)):
                raise ValueError("prebuilt compiler requires a lowercase SHA-256 digest")
            compiler = prebuilt_compiler.resolve(strict=True)
            if not compiler.is_file():
                raise ValueError("prebuilt compiler is missing")
            with compiler.open("rb") as stream:
                actual = hashlib.file_digest(stream, "sha256").hexdigest()
            if actual != compiler_sha256:
                raise ValueError("prebuilt compiler checksum mismatch")
            # Run the verified executable in-place. Windows may retain an image
            # section handle after subprocess.run() returns (WinError 5), so a
            # transient copy in the private output staging directory cannot
            # reliably be unlinked. The kit executable is never overwritten,
            # copied, or deleted here.
            print("Using verified prebuilt native ARM32-to-ARM64 translator", flush=True)
        print("Validating and importing original ZIP locally", flush=True)
        with open(os.devnull, "w", encoding="utf-8") as sink:
            with contextlib.redirect_stdout(sink):
                install_release(zip_path, ROOT, assets)
        # Use the same XLIFF, font and formatter compiler as the normal Mac
        # build. The original ZIP has no community translation by itself.
        print("Preparing configured localizations", flush=True)
        locale_header = staging / "localization_config.h"
        enabled = prepare_localizations(ROOT, assets, locale_header,
                                        translations_root=translations_root)
        if not enabled:
            raise ValueError("no configured localization was generated")
        print("Localizations:", ", ".join(item.code for item in enabled), flush=True)
        print("Generating native ARM64 blocks", flush=True)
        compiled = staging / "compiled"
        compiled.mkdir()
        cmd = [
            str(compiler), str(assets / "lib/libdespicablemefree.so"),
            *(str(compiled / name) for name in AOT_PRODUCTS),
        ]
        subprocess.run(cmd, check=True, timeout=600)
        for name in AOT_PRODUCTS:
            if not (compiled / name).is_file() or not (compiled / name).stat().st_size:
                raise ValueError(f"AOT translator failed to create: {name}")
        # Only a host-compiled, staging-owned executable is ephemeral.
        # A verified prebuilt compiler belongs to the kit and must not be
        # removed, particularly on Windows with executable image locks.
        if owned_compiler:
            compiler.unlink()
        sizes = {name: (compiled / name).stat().st_size for name in AOT_PRODUCTS}
        checksums = {}
        for name in AOT_PRODUCTS:
            with (compiled / name).open("rb") as stream:
                checksums[name] = hashlib.file_digest(stream, "sha256").hexdigest()
        details = {
            "status": "private-aot-only",
            "release": "1.8.1g",
            "native_execution": "ARM64 blocks; no JIT",
            "not_an_ipa": True,
            "products_sha256": checksums,
            "products_bytes": sizes,
        }
        (staging / "aot-manifest.json").write_text(
            json.dumps(details, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        staging.rename(destination)
    return details


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_zip", type=Path, help="user-owned original 1.8.1g ZIP")
    parser.add_argument("--output", type=Path, required=True,
                        help="absolute, NEW private output directory")
    parser.add_argument("--cc", default="clang", help="host C compiler (default: clang)")
    parser.add_argument("--prebuilt-compiler", type=Path,
                        help="trusted prebuilt Windows AOT executable")
    parser.add_argument("--compiler-sha256", help="expected SHA-256 of prebuilt executable")
    args = parser.parse_args()
    try:
        result = prepare(args.source_zip, args.output, cc=args.cc,
                         prebuilt_compiler=args.prebuilt_compiler,
                         compiler_sha256=args.compiler_sha256)
    except (ValueError, OSError, zipfile.BadZipFile, subprocess.SubprocessError) as exc:
        print(f"AOT preparation failed: {exc}", file=sys.stderr)
        return 1
    print(f"Private native ARM64 translation complete: {args.output}")
    print(f"{len(result['products_sha256'])} generated files; no IPA produced.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
