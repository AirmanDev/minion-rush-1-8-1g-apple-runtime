#!/usr/bin/env python3
"""Run a private, offline Windows ZIP -> native AOT -> unsigned IPA build.

Distributed tools, pretranslated metadata and the template are version-locked.
This helper NEVER signs or installs the resulting IPA on an iPhone.
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

from asset_contract import OFFICIAL_ENGINE_SHA256
from common import ROOT, public_files
from native_ipa import assemble_unsigned_ipa
from native_macho import file_sha256
from prepare_native_aot import prepare


def validate_kit(kit: Path) -> dict:
    if not kit.is_dir() or kit.is_symlink():
        raise ValueError("portable Windows kit directory is missing or linked")
    manifest_path = kit / "kit-manifest.json"
    if not manifest_path.is_file() or manifest_path.stat().st_size > 4096:
        raise ValueError("Windows kit manifest is missing or invalid")
    config = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(config, dict) or set(config) != {
        "version", "engine_sha256", "template_sha256", "compiler_sha256",
        "runtime_manifest_sha256", "runtime_files_sha256", "python_zip_sha256",
    } or config["version"] != 1:
        raise ValueError("Windows kit manifest format is invalid")
    if config["engine_sha256"] != OFFICIAL_ENGINE_SHA256:
        raise ValueError("Windows kit targets the wrong game engine")
    for label in ("template_sha256", "compiler_sha256",
                  "runtime_manifest_sha256", "runtime_files_sha256", "python_zip_sha256"):
        digest = config[label]
        if (not isinstance(digest, str) or len(digest) != 64
                or any(char not in "0123456789abcdef" for char in digest)):
            raise ValueError("invalid Windows kit checksum")
    runtime_root = ROOT.resolve()
    paths = {
        "template_sha256": kit / "MinionRush-native-template-unsigned.zip",
        "compiler_sha256": kit / "a64-compiler.exe",
        "runtime_manifest_sha256": runtime_root / "config/source_manifest.txt",
        "runtime_files_sha256": kit / "runtime-file-hashes.json",
        "python_zip_sha256": kit / "python/python314.zip",
    }
    for name, path in paths.items():
        if not path.is_file() or path.is_symlink():
            raise ValueError("Windows kit file is missing: " + name)
        if file_sha256(path) != config[name]:
            raise ValueError("Windows kit checksum mismatch: " + name)
    reference = json.loads((kit / "runtime-file-hashes.json").read_text(encoding="utf-8"))
    if not isinstance(reference, dict):
        raise ValueError("invalid Windows runtime file hashes")
    current = {
        path.relative_to(runtime_root).as_posix(): file_sha256(path)
        for path in public_files(runtime_root)
    }
    if reference != current:
        raise ValueError("Windows runtime source integrity verification failed")
    return config


def create_unsigned_ipa(zip_path: Path, kit: Path, output: Path) -> None:
    if output.exists() or output.is_symlink():
        raise ValueError("refusing to overwrite an existing unsigned IPA")
    if not output.is_absolute() or not output.parent.is_dir():
        raise ValueError("output must be in an existing absolute directory")
    kit = kit.resolve(strict=True)
    config = validate_kit(kit)
    with tempfile.TemporaryDirectory(prefix="minion-rush-private-", dir=None) as tmp:
        private = Path(tmp) / "private-AOT"
        print("1/3: Validate your own 1.8.1g ZIP and prepare native ARM64 blocks.", flush=True)
        prepare(
            zip_path, private,
            prebuilt_compiler=kit / "a64-compiler.exe",
            compiler_sha256=config["compiler_sha256"],
        )
        print("2/3: Assemble the unsigned IPA and embed original game data.", flush=True)
        assemble_unsigned_ipa(
            kit / "MinionRush-native-template-unsigned.zip", private, output
        )
    print("3/3: Native unsigned IPA assembled and verified.", flush=True)
    print("File:", output, flush=True)
    print("IMPORTANT: The IPA must be signed and installed with a compatible", flush=True)
    print("external sideloading tool using your own Apple account.", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_zip", type=Path, help="your own original 1.8.1g ZIP")
    parser.add_argument("--kit", type=Path, required=True, help="portable Windows kit folder")
    parser.add_argument("--output", type=Path, required=True, help="new unsigned IPA")
    args = parser.parse_args()
    try:
        create_unsigned_ipa(args.source_zip, args.kit, args.output)
    except (ValueError, OSError, RuntimeError, json.JSONDecodeError) as exc:
        print("Windows native IPA build failed:", exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
