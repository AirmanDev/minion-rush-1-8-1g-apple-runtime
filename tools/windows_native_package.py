#!/usr/bin/env python3
"""Run a private, offline Windows ZIP -> native AOT -> unsigned IPA build.

Distributed tools, pretranslated metadata and the template are version-locked.
This helper NEVER signs or installs the resulting IPA on an iPhone.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path, PureWindowsPath

sys.dont_write_bytecode = True

from asset_contract import OFFICIAL_ENGINE_SHA256
from install_assets import MAX_ARCHIVE_BYTES
from common import public_files
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
        "python_files_sha256",
    } or config["version"] != 1:
        raise ValueError("Windows kit manifest format is invalid")
    if config["engine_sha256"] != OFFICIAL_ENGINE_SHA256:
        raise ValueError("Windows kit targets the wrong game engine")
    for label in ("template_sha256", "compiler_sha256",
                  "runtime_manifest_sha256", "runtime_files_sha256", "python_zip_sha256",
                  "python_files_sha256"):
        digest = config[label]
        if (not isinstance(digest, str) or len(digest) != 64
                or any(char not in "0123456789abcdef" for char in digest)):
            raise ValueError("invalid Windows kit checksum")
    if (kit / "runtime").is_symlink():
        raise ValueError("linked Windows runtime directory")
    runtime_root = (kit / "runtime").resolve()
    paths = {
        "template_sha256": kit / "MinionRush-native-template-unsigned.zip",
        "compiler_sha256": kit / "a64-compiler.exe",
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


def _mapped_unc_root(letter: str) -> str | None:
    """Return the actual mapped Windows network resource, never guess from Z:."""
    if os.name != "nt" or not re.fullmatch(r"[A-Za-z]:", letter):
        return None
    import ctypes
    from ctypes import wintypes

    buffer = ctypes.create_unicode_buffer(32768)
    capacity = wintypes.DWORD(len(buffer))
    result = ctypes.windll.mpr.WNetGetConnectionW(
        letter, buffer, ctypes.byref(capacity)
    )
    return buffer.value if result == 0 else None


def _utm_webdav_url(source: Path) -> str | None:
    """Restrict direct HTTP access to the UTM SPICE server on guest loopback.

    Windows WebClient's mapped drive may refuse to open ZIPs larger than its
    configured file-size limit. The separate UTM SPICE WebDAV service handles
    ordinary HTTP GET without invoking that Windows filesystem redirector.
    """
    if os.name != "nt":
        return None
    path = PureWindowsPath(str(source))
    if path.drive.startswith("\\\\"):
        remote = path.drive
    else:
        remote = _mapped_unc_root(path.drive)
    if not remote or remote.lower().rstrip("\\") != r"\\localhost@9843\davwwwroot":
        return None
    parts = path.parts[1:]
    if (not parts or any(part in ("", ".", "..") for part in parts)
            or any("\x00" in part or "/" in part or "\\" in part for part in parts)):
        raise ValueError("unsafe UTM shared ZIP path")
    encoded = "/".join(urllib.parse.quote(part, safe="") for part in parts)
    return "http://127.0.0.1:9843/" + encoded


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        return None


def _utm_http_stream(url: str):
    """No proxies, cookies, credentials, redirects, or non-loopback hosts."""
    if not url.startswith("http://127.0.0.1:9843/"):
        raise ValueError("unsafe UTM share URL")
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({}), _NoRedirect()
    )
    response = opener.open(
        urllib.request.Request(url, method="GET"), timeout=60
    )
    length = response.headers.get("Content-Length")
    if length is not None:
        try:
            size = int(length)
        except ValueError as exc:
            response.close()
            raise ValueError("invalid UTM HTTP ZIP length") from exc
        if size <= 0 or size > MAX_ARCHIVE_BYTES:
            response.close()
            raise ValueError("UTM HTTP ZIP exceeds supported size")
    return response


def _stage_source_archive(source: Path, temporary: Path) -> Path:
    """Copy remote ZIP sequentially to private local scratch, never to the kit.

    UTM's SPICE WebDAV drive is accessible as a UNC path but does not reliably
    implement random-access file operations required by Python's zipfile.
    """
    if source.suffix.lower() != ".zip":
        raise ValueError("original source must be a ZIP archive")
    if not _needs_local_archive_copy(source):
        return source
    destination = temporary / "original-private-source.zip"
    if destination.exists() or destination.is_symlink():
        raise ValueError("private ZIP scratch location already exists")
    total = 0
    print("Copying shared ZIP to temporary Windows storage for reliable reading.", flush=True)
    # UTM's Win32 mapped WebDAV redirector rejects large file opens. HTTP GET
    # against the guest's loopback-only SPICE service bypasses that limitation.
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

def create_unsigned_ipa(zip_path: Path, kit: Path, output: Path) -> None:
    if output.exists() or output.is_symlink():
        raise ValueError("refusing to overwrite an existing unsigned IPA")
    if not output.is_absolute() or not output.parent.is_dir():
        raise ValueError("output must be in an existing absolute directory")
    kit = kit.resolve(strict=True)
    config = validate_kit(kit)
    translations_root = kit.parent / "MinionRush-Private-Languages"
    if translations_root.is_symlink() or not translations_root.is_dir():
        raise ValueError(
            "private Hungarian catalog is missing: supply "
            "MinionRush-Private-Languages/hu.xlf next to the Windows kit"
        )
    if not (translations_root / "hu.xlf").is_file():
        raise ValueError("private hu.xlf localization catalog is missing")
    with tempfile.TemporaryDirectory(prefix="minion-rush-private-", dir=None) as tmp:
        temporary = Path(tmp)
        private = temporary / "private-AOT"
        print("1/3: Validate your own 1.8.1g ZIP and prepare native ARM64 blocks.", flush=True)
        local_zip = _stage_source_archive(zip_path, temporary)
        prepare(
            local_zip, private,
            prebuilt_compiler=kit / "a64-compiler.exe",
            compiler_sha256=config["compiler_sha256"],
            translations_root=translations_root,
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
