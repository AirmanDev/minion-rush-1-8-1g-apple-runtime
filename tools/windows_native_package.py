#!/usr/bin/env python3
"""Run a private, offline Windows ZIP -> native AOT -> unsigned IPA build.

Distributed tools, pretranslated metadata and the template are version-locked.
This helper NEVER signs or installs the resulting IPA on an iPhone.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

from asset_contract import OFFICIAL_ENGINE_SHA256
from install_assets import MAX_ARCHIVE_BYTES
from common import public_files
from native_ipa import assemble_unsigned_ipa
from native_ipa_output import assemble_output, validate_output_destination
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
        "version", "mode", "hungarian_xlf_sha256", "engine_sha256", "template_sha256", "compiler_sha256", "gui_sha256",
        "runtime_manifest_sha256", "runtime_files_sha256", "python_zip_sha256",
        "python_files_sha256",
    } or config["version"] != 5 or config["mode"] not in ("release", "utm-test"):
        raise ValueError("Windows kit manifest format is invalid")
    if config["engine_sha256"] != OFFICIAL_ENGINE_SHA256:
        raise ValueError("Windows kit targets the wrong game engine")
    for label in ("template_sha256", "compiler_sha256", "gui_sha256",
                  "runtime_manifest_sha256", "runtime_files_sha256", "python_zip_sha256",
                  "python_files_sha256"):
        digest = config[label]
        if (not isinstance(digest, str) or len(digest) != 64
                or any(char not in "0123456789abcdef" for char in digest)):
            raise ValueError("invalid Windows kit checksum")
    catalog_digest = config["hungarian_xlf_sha256"]
    catalog_file = kit / "private-localizations" / "hu.xlf"
    if catalog_digest is not None:
        if (not isinstance(catalog_digest, str) or len(catalog_digest) != 64 or
                any(char not in "0123456789abcdef" for char in catalog_digest)):
            raise ValueError("invalid bundled catalog checksum")
        if (not catalog_file.is_file() or catalog_file.is_symlink() or
                catalog_file.stat().st_size > 2 * 1024 * 1024 or
                file_sha256(catalog_file) != catalog_digest):
            raise ValueError("bundled Hungarian catalog checksum mismatch")
    if (kit / "private-localizations").exists():
        if (catalog_digest is None or
                {path.name for path in (kit / "private-localizations").iterdir()} != {"hu.xlf"}):
            raise ValueError("unexpected private language files in kit")
    if (kit / "runtime").is_symlink():
        raise ValueError("linked Windows runtime directory")
    runtime_root = (kit / "runtime").resolve()
    paths = {
        "template_sha256": kit / "MinionRush-native-template-unsigned.zip",
        "compiler_sha256": kit / "a64-compiler.exe",
        "gui_sha256": kit / "MinionRush Builder.exe",
        "runtime_manifest_sha256": runtime_root / "config/source_manifest.txt",
        "runtime_files_sha256": kit / "runtime-file-hashes.json",
        "python_zip_sha256": kit / "python/python314.zip",
        "python_files_sha256": kit / "python-file-hashes.json",
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
    utm_file = runtime_root / "tools/windows_utm_transport.py"
    if utm_file.is_file() != (config["mode"] == "utm-test"):
        raise ValueError("Windows kit profile does not match the bundled transport")
    # Reject any additional runtime files: an attacker cannot add an unchecked
    # module in the import directory or swap a verified Python extension.
    if not runtime_root.is_dir() or any(p.is_symlink() for p in runtime_root.rglob("*")):
        raise ValueError("Windows runtime contains an invalid symlink")
    actual_runtime_files = {
        p.relative_to(runtime_root).as_posix() for p in runtime_root.rglob("*")
        if p.is_file()
    }
    if actual_runtime_files != set(reference):
        raise ValueError("Windows runtime contains missing or extra files")
    python_root = kit / "python"
    if not python_root.is_dir() or python_root.is_symlink():
        raise ValueError("Windows embedded Python directory is missing or linked")
    python_manifest = json.loads((kit / "python-file-hashes.json").read_text(encoding="utf-8"))
    if not isinstance(python_manifest, dict) or not python_manifest:
        raise ValueError("Windows embedded Python inventory is invalid")
    if any(p.is_symlink() or not p.is_file() for p in python_root.iterdir()):
        raise ValueError("Windows embedded Python contains unexpected file types")
    actual_python = {
        path.name: file_sha256(path) for path in python_root.iterdir()
    }
    if actual_python != python_manifest:
        raise ValueError("Windows embedded Python integrity verification failed")
    if (python_root / "python314._pth").read_bytes() != (
        b"python314.zip\n.\n..\\runtime\\tools\n"
    ):
        raise ValueError("unsafe embedded Python import configuration")
    return config



def _needs_local_archive_copy(source: Path) -> bool:
    """A Windows network share may not support ZIP seek() and backward reads."""
    if os.name != "nt":
        return False
    if str(source).startswith("\\\\"):
        return True
    # Also support mapped network drives, not just raw WebDAV/SMB UNC paths.
    import ctypes
    anchor = source.anchor
    return bool(anchor) and ctypes.windll.kernel32.GetDriveTypeW(anchor) == 4


def _stage_source_archive(source: Path, temporary: Path, *, mode: str = "release") -> Path:
    """Copy remote ZIP sequentially to private local scratch, never to the kit.

    Network drives may not reliably implement random access. Only explicit
    UTM test kits use the guest-local SPICE HTTP transport.
    """
    if mode not in ("release", "utm-test"):
        raise ValueError("unsupported Windows import mode")
    if source.suffix.lower() != ".zip":
        raise ValueError("original source must be a ZIP archive")
    if not _needs_local_archive_copy(source):
        return source
    destination = temporary / "original-private-source.zip"
    if destination.exists() or destination.is_symlink():
        raise ValueError("private ZIP scratch location already exists")
    total = 0
    print("Copying shared ZIP to temporary Windows storage for reliable reading.", flush=True)
    url = None
    if mode == "utm-test":
        from windows_utm_transport import _utm_webdav_url, _utm_http_stream
        url = _utm_webdav_url(source)
    print("Import transport:", "UTM local WebDAV HTTP" if url else "filesystem", flush=True)
    incoming = _utm_http_stream(url) if url else source.open("rb")
    expected_size = int(incoming.headers["Content-Length"]) if url and incoming.headers.get(
        "Content-Length"
    ) is not None else None
    with incoming, destination.open("xb") as outgoing:
        while chunk := incoming.read(1024 * 1024):
            total += len(chunk)
            if total > MAX_ARCHIVE_BYTES:
                raise ValueError("original source ZIP exceeds supported size")
            outgoing.write(chunk)
        outgoing.flush()
        os.fsync(outgoing.fileno())
    if total == 0:
        raise ValueError("original source ZIP is empty")
    if expected_size is not None and total != expected_size:
        raise ValueError("incomplete UTM HTTP ZIP download")
    print("Original ZIP staged privately; validating official game contents.", flush=True)
    return destination

def create_unsigned_ipa(zip_path: Path, kit: Path, output: Path,
                        hungarian_xlf: Path | None = None,
                        scratch_root: Path | None = None,
                        include_hungarian: bool = True,
                        overwrite: bool = False) -> None:
    validate_output_destination(output, overwrite=overwrite)
    kit = kit.resolve(strict=True)
    config = validate_kit(kit)
    translations_root = None
    if not include_hungarian and hungarian_xlf is not None:
        raise ValueError("explicit Hungarian catalog conflicts with disabled language")
    if not include_hungarian:
        print("Additional languages: none (original game languages only)", flush=True)
    elif hungarian_xlf is not None:
        selected = hungarian_xlf
        if (selected.name.lower() != "hu.xlf" or not selected.is_file()
                or selected.is_symlink()):
            raise ValueError("expected a private, regular hu.xlf catalog")
        translations_root = selected.parent
    else:
        bundled = kit / "private-localizations" / "hu.xlf"
        optional = (bundled if config.get("hungarian_xlf_sha256") is not None
                    else kit.parent / "MinionRush-Private-Languages" / "hu.xlf")
        if optional.is_file() and not optional.is_symlink():
            translations_root = optional.parent
    if scratch_root is not None:
        if (not scratch_root.is_absolute() or scratch_root.is_symlink()
                or not scratch_root.is_dir()):
            raise ValueError("invalid private scratch directory")
    # Keep the staged source and generated files on GUI failures only. In
    # normal CLI mode, cleanup is unconditional, including on exceptions.
    temporary = Path(tempfile.mkdtemp(prefix="mr-", dir=scratch_root))
    completed = False
    try:
        private = temporary / "private-AOT"
        print("1/3: Validate your own 1.8.1g ZIP and prepare native ARM64 blocks.", flush=True)
        local_zip = _stage_source_archive(zip_path, temporary, mode=config.get("mode", "release"))
        prepare(
            local_zip, private,
            prebuilt_compiler=kit / "a64-compiler.exe",
            compiler_sha256=config["compiler_sha256"],
            translations_root=translations_root,
            enable_localizations=include_hungarian,
        )
        print("2/3: Assemble the unsigned IPA and embed original game data.", flush=True)
        template = kit / "MinionRush-native-template-unsigned.zip"
        assemble_output(
            output, lambda candidate: assemble_unsigned_ipa(template, private, candidate),
            overwrite=overwrite,
        )
        completed = True
    finally:
        if completed or scratch_root is None:
            shutil.rmtree(temporary)
        else:
            print("Private diagnostic files retained in:", temporary, flush=True)
    print("3/3: Native unsigned IPA assembled and verified.", flush=True)
    print("File:", output, flush=True)
    print("IMPORTANT: The IPA must be signed and installed with a compatible", flush=True)
    print("external sideloading tool using your own Apple account.", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_zip", type=Path, help="your own original 1.8.1g ZIP")
    parser.add_argument("--kit", type=Path, required=True, help="portable Windows kit folder")
    parser.add_argument("--output", type=Path, required=True, help="new unsigned IPA")
    parser.add_argument("--hungarian-xlf", type=Path,
                        help="optional private hu.xlf translation catalog")
    parser.add_argument("--no-hungarian", action="store_true",
                        help="exclude the bundled Hungarian language from this IPA")
    parser.add_argument("--overwrite", action="store_true",
                        help="replace a confirmed existing IPA after successful assembly")
    parser.add_argument("--scratch-root", type=Path,
                        help="private per-job directory managed by the graphical launcher")
    args = parser.parse_args()
    try:
        create_unsigned_ipa(args.source_zip, args.kit, args.output,
                            hungarian_xlf=args.hungarian_xlf,
                            scratch_root=args.scratch_root,
                            include_hungarian=not args.no_hungarian,
                            overwrite=args.overwrite)
    except (ValueError, OSError, RuntimeError, json.JSONDecodeError) as exc:
        print("Windows native IPA build failed:", exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
