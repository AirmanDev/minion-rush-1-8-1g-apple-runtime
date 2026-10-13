#!/usr/bin/env python3
"""Unsigned native iOS template generation and private Windows IPA assembly.

The developer builds an APP WITHOUT original game assets/code on macOS.
Each user privately inserts their own translated ARM64 block code, restores
their verified assets, then submits the unsigned IPA to an external signer.
No signing identities, SDKs, originals, or network uploads are involved.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import plistlib
import shutil
import stat
import struct
import sys
import tempfile
import unicodedata
import zipfile
from pathlib import Path, PurePosixPath

sys.dont_write_bytecode = True

from common import ROOT, content_files
from ipa_archive import export_ipa
from ios_private_icon import install_private_icon
from native_macho import file_sha256, find_code_slot, require_zero_slot
from native_profile import VERSION as PROFILE_VERSION, create_profile, inspect_profile
from validate_assets import inspect_assets

APP_NAME = "MinionRush.app"
EXE_NAME = "MinionRush"
TEMPLATE_PREFIX = "Template/" + APP_NAME + "/"
TEMPLATE_MANIFEST = "template-manifest.json"
VERSION = 3
MAX_TEMPLATE_BYTES = 350 * 1024 * 1024
MAX_TEMPLATE_MEMBERS = 512
MAX_PRIVATE_ASSETS = 2 * 1024 * 1024 * 1024
PRIVATE_TOP_LEVEL = frozenset(("lib", "game", "localizations"))
SIGNING_FILES = frozenset(("embedded.mobileprovision", "_CodeSignature", "SC_Info"))
ALLOWED_TEMPLATE_FILES = frozenset((
    "MinionRush", "Info.plist", "PrivacyInfo.xcprivacy", "PkgInfo",
))



def _safe_name(name: str) -> str:
    if not name or "\\" in name or "\0" in name or name.startswith("/") or name.endswith("/"):
        raise ValueError("invalid template ZIP member")
    posix = PurePosixPath(name)
    if any(part in ("", ".", "..") for part in name.split("/")) or ":" in posix.parts[0]:
        raise ValueError("unsafe template ZIP member")
    if posix.as_posix() != name:
        raise ValueError("unnormalized template ZIP member")
    return unicodedata.normalize("NFC", name).casefold()


def _validate_bundle_relative(relative: Path) -> None:
    if not relative.parts or any(part.startswith(".") for part in relative.parts):
        raise ValueError("hidden/metadata files cannot enter the runtime template")
    if any(part in SIGNING_FILES for part in relative.parts):
        raise ValueError("signing files cannot enter an unsigned template")
    if relative.parts[0] in PRIVATE_TOP_LEVEL:
        raise ValueError("game files cannot enter a public runtime template")
    if relative.as_posix() not in ALLOWED_TEMPLATE_FILES:
        raise ValueError("unexpected file in native runtime template")


def _check_template_zip(archive: zipfile.ZipFile) -> dict[str, zipfile.ZipInfo]:
    infos = archive.infolist()
    if not infos or len(infos) > MAX_TEMPLATE_MEMBERS:
        raise ValueError("too many entries in unsigned template")
    if sum(i.file_size for i in infos) > MAX_TEMPLATE_BYTES:
        raise ValueError("unsigned template exceeds size limit")
    found: dict[str, zipfile.ZipInfo] = {}
    normalized: set[str] = set()
    for info in infos:
        name = info.filename
        canonical = _safe_name(name)
        mode = info.external_attr >> 16
        if (stat.S_IFMT(mode) not in (0, stat.S_IFREG) or info.is_dir()
                or info.flag_bits & 0x1):
            raise ValueError("unsafe template entry type or encryption")
        if canonical in normalized:
            raise ValueError("duplicated template member")
        normalized.add(canonical)
        if name != TEMPLATE_MANIFEST:
            if not name.startswith(TEMPLATE_PREFIX):
                raise ValueError("unexpected file outside the app bundle")
            relative = Path(name[len(TEMPLATE_PREFIX):])
            _validate_bundle_relative(relative)
        found[name] = info
    if TEMPLATE_MANIFEST not in found or TEMPLATE_PREFIX + EXE_NAME not in found:
        raise ValueError("unsigned template is incomplete")
    return found


def _collect_app_files(application: Path) -> list[tuple[Path, Path]]:
    if application.is_symlink() or not application.is_dir() or application.name != APP_NAME:
        raise ValueError("a valid unsigned MinionRush.app is required")
    paths = []
    total = 0
    for child in sorted(application.rglob("*")):
        if child.is_symlink():
            raise ValueError("symbolic links are forbidden in template bundles")
        if child.is_dir():
            continue
        if not child.is_file():
            raise ValueError("unexpected template file type")
        relative = child.relative_to(application)
        _validate_bundle_relative(relative)
        total += child.stat().st_size
        paths.append((child, relative))
    if total > MAX_TEMPLATE_BYTES or len(paths) > MAX_TEMPLATE_MEMBERS - 1:
        raise ValueError("template application exceeds limits")
    return paths


def _validate_manifest(data: object) -> dict:
    if not isinstance(data, dict) or set(data) != {
        "version", "app_name", "executable_sha256", "slot_offset", "slot_size",
        "profile_version",
    }:
        raise ValueError("invalid generic template manifest")
    if (data["version"] != VERSION or data["app_name"] != APP_NAME
            or data["profile_version"] != PROFILE_VERSION):
        raise ValueError("unsupported generic template version")
    if (not isinstance(data["slot_offset"], int) or not isinstance(data["slot_size"], int)
            or data["slot_offset"] < 0 or not 64 <= data["slot_size"] <= 256*1024*1024):
        raise ValueError("invalid template code slot")
    digest = data["executable_sha256"]
    if (not isinstance(digest, str) or len(digest) != 64
            or any(char not in "0123456789abcdef" for char in digest)):
        raise ValueError("invalid executable digest")
    return data


def _validate_app_info(data: bytes) -> None:
    payload = plistlib.loads(data)
    if not isinstance(payload, dict) or payload.get("CFBundleExecutable") != EXE_NAME:
        raise ValueError("unexpected executable in runtime template")
    identifier = payload.get("CFBundleIdentifier")
    if not isinstance(identifier, str) or not identifier or len(identifier) > 255:
        raise ValueError("unsigned template lacks a bundle identifier")
    for key in ("CFBundleIcons", "CFBundleIcons~ipad", "CFBundleIconFiles", "CFBundleIconName"):
        if key in payload:
            raise ValueError("unsigned template must not contain app icon metadata")


def create_template(application: Path, output: Path) -> dict:
    """Pack a verified unsigned, asset-free template built using Apple SDK."""
    if output.exists() or output.is_symlink():
        raise ValueError("refusing to overwrite an existing template archive")
    if not output.parent.is_dir():
        raise ValueError("template output directory does not exist")
    files = _collect_app_files(application)
    bundle = {relative.as_posix(): path for path, relative in files}
    if EXE_NAME not in bundle or "Info.plist" not in bundle:
        raise ValueError("unsigned runtime bundle lacks executable or Info.plist")
    _validate_app_info(bundle["Info.plist"].read_bytes())
    binary = bundle[EXE_NAME].read_bytes()
    slot = find_code_slot(binary)
    require_zero_slot(binary, slot)
    manifest = _validate_manifest({
        "version": VERSION,
        "app_name": APP_NAME,
        "executable_sha256": hashlib.sha256(binary).hexdigest(),
        "slot_offset": slot.offset,
        "slot_size": slot.size,
        "profile_version": PROFILE_VERSION,
    })
    fd, staging = tempfile.mkstemp(prefix=".mr-template-", suffix=".zip", dir=output.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
                archive.writestr(TEMPLATE_MANIFEST, json.dumps(manifest, sort_keys=True, indent=2)+"\n")
                for src, relative in files:
                    archive.write(src, TEMPLATE_PREFIX+relative.as_posix())
            stream.flush()
            os.fsync(stream.fileno())
        with zipfile.ZipFile(staging) as archive:
            _check_template_zip(archive)
        os.replace(staging, output)
    finally:
        Path(staging).unlink(missing_ok=True)
    return manifest


def _unpack_template(template: Path, staging: Path) -> tuple[Path, dict]:
    if not template.is_file():
        raise ValueError("unsigned template ZIP is missing")
    with zipfile.ZipFile(template) as archive:
        found = _check_template_zip(archive)
        manifest_info = found[TEMPLATE_MANIFEST]
        if manifest_info.file_size > 4096:
            raise ValueError("template manifest is oversized")
        manifest = _validate_manifest(json.loads(archive.read(manifest_info)))
        app = staging / APP_NAME
        app.mkdir()
        for name, info in found.items():
            if name == TEMPLATE_MANIFEST:
                continue
            relative = Path(name[len(TEMPLATE_PREFIX):])
            target = app / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, target.open("wb") as sink:
                shutil.copyfileobj(source, sink, length=1024 * 1024)
        executable = app / EXE_NAME
        _validate_app_info((app / "Info.plist").read_bytes())
        binary = executable.read_bytes()
        if hashlib.sha256(binary).hexdigest() != manifest["executable_sha256"]:
            raise ValueError("unsigned template executable checksum mismatch")
        slot = find_code_slot(binary)
        if (slot.offset, slot.size) != (manifest["slot_offset"], manifest["slot_size"]):
            raise ValueError("Mach-O code slot is inconsistent with template manifest")
        require_zero_slot(binary, slot)
        executable.chmod(0o755)
    return app, manifest


def _validate_private_assets(assets: Path) -> list[tuple[Path, Path]]:
    report = inspect_assets(ROOT, assets)
    if report.bytes > MAX_PRIVATE_ASSETS:
        raise ValueError("private assets exceed size limit")
    files: list[tuple[Path, Path]] = []
    paths: set[str] = set()
    for path in sorted(assets.rglob("*")):
        if path.is_symlink():
            raise ValueError("private game assets may not contain symbolic links")
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError("unsupported asset file type")
        relative = path.relative_to(assets)
        if relative.parts[0] not in PRIVATE_TOP_LEVEL:
            # The original launcher artwork is converted into private icons later.
            if relative.as_posix() == "app-icon.png":
                continue
            raise ValueError("unexpected private asset location")
        key = unicodedata.normalize("NFC", relative.as_posix()).casefold()
        if key in paths or any(part.startswith(".") for part in relative.parts):
            raise ValueError("unsafe or duplicated game asset path")
        paths.add(key)
        files.append((path, relative))
    return files


def assemble_unsigned_ipa(template: Path, private_aot: Path, output: Path) -> None:
    """Create an unsigned per-user IPA, without changing the Mac build."""
    if output.suffix.casefold() != ".ipa" or output.exists() or output.is_symlink():
        raise ValueError("output must be a new .ipa file")
    if not output.parent.is_dir() or output.parent.is_symlink():
        raise ValueError("invalid output parent directory")
    compiled = private_aot / "compiled"
    assets = private_aot / "assets"
    if not private_aot.is_dir() or private_aot.is_symlink():
        raise ValueError("private AOT directory is missing or linked")
    profile = create_profile(compiled, ROOT / "src/native/game_symbols.def")
    profile_info = inspect_profile(profile, ROOT / "src/native/game_symbols.def")
    game_assets = _validate_private_assets(assets)
    with tempfile.TemporaryDirectory(prefix=".mr-ipa-", dir=output.parent) as directory:
        app, manifest = _unpack_template(template, Path(directory))
        codefile = compiled / "minion_code.bin"
        if (codefile.stat().st_size != profile_info["code_size"]
                or codefile.stat().st_size > manifest["slot_size"]):
            raise ValueError("native ARM64 code does not fit generic template slot")
        executable = app / EXE_NAME
        with executable.open("r+b") as destination, codefile.open("rb") as source:
            destination.seek(manifest["slot_offset"])
            shutil.copyfileobj(source, destination, length=1024 * 1024)
            if source.read(1):
                raise ValueError("native code is longer than reserved executable slot")
            destination.flush()
            os.fsync(destination.fileno())
        # Ensure executable bytes were replaced at the expected offset.
        with executable.open("rb") as stream:
            stream.seek(manifest["slot_offset"])
            hasher = hashlib.sha256()
            remaining = profile_info["code_size"]
            while remaining:
                block = stream.read(min(1024*1024, remaining))
                if not block:
                    raise ValueError("short native code image")
                hasher.update(block)
                remaining -= len(block)
        if hasher.hexdigest() != file_sha256(codefile):
            raise ValueError("native code section digest mismatch after assembly")
        (app / "game-profile.bin").write_bytes(profile)
        for original, relative in game_assets:
            target = app / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(original, target)
        install_private_icon(app, assets / "app-icon.png")
        export_ipa(app, output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="action", required=True)
    template = subs.add_parser("create-template")
    template.add_argument("--app", type=Path, required=True)
    template.add_argument("--output", type=Path, required=True)
    assembler = subs.add_parser("assemble")
    assembler.add_argument("--template", type=Path, required=True)
    assembler.add_argument("--private-aot", type=Path, required=True)
    assembler.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.action == "create-template":
            create_template(args.app, args.output)
        else:
            assemble_unsigned_ipa(args.template, args.private_aot, args.output)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, plistlib.InvalidFileException) as exc:
        print("native IPA error:", exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
