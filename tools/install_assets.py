#!/usr/bin/env python3
"""Rebuild assets/ from the official, unmodified Minion Rush 1.8.1g release.

The source is the distribution archive (or an already extracted copy of it)
that contains the signed APKs and one `data/com.gameloft.android.ANMP.GloftDMHM`
directory.  Several APKs in that archive carry patched engines; the official one
is picked by the SHA-256 of its ARM32 engine, so a modified build can never be
installed by accident.

Assets are staged and validated before replacing an existing installation.
There is no merge step or state carried over from a previous asset install.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import stat
import sys
import tempfile
import unicodedata
import zipfile
from pathlib import Path
from pathlib import PurePosixPath

sys.dont_write_bytecode = True

from common import content_files, load_json_object
from configure_graphics import ENCODING, apply_configuration, write_profiles
from asset_contract import OFFICIAL_APP_ICON_SHA256, OFFICIAL_ENGINE_SHA256
from validate_assets import AssetReport, inspect_assets

PACKAGE = "com.gameloft.android.ANMP.GloftDMHM"
ENGINE_IN_APK = "lib/armeabi-v7a/libdespicablemefree.so"
APP_ICON_IN_APK = "res/drawable-xxxhdpi/icon.png"

# The DLC layer's own diagnostic log. It is not game data, so a foreign copy is
# never imported -- but it is not disposable either: MR_GUEST_LOG=1 asks the
# runtime to write one, and no validator may then delete it behind the user.
NOT_INSTALLED = frozenset({"manhattan.log"})
MAX_ARCHIVE_BYTES = 2 * 1024 * 1024 * 1024
MAX_ARCHIVE_MEMBERS = 20_000
MAX_ENGINE_BYTES = 64 * 1024 * 1024
MAX_APP_ICON_BYTES = 4 * 1024 * 1024


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def extract_release_archive(archive: zipfile.ZipFile, destination: Path) -> None:
    """Extract a bounded ZIP without links or paths outside destination."""
    members = archive.infolist()
    if len(members) > MAX_ARCHIVE_MEMBERS:
        raise ValueError(f"too many entries in ZIP: {len(members)}")
    total = sum(member.file_size for member in members)
    if total > MAX_ARCHIVE_BYTES:
        raise ValueError(f"extracted ZIP is too large: {total} bytes")

    print(f"ZIP extraction: {len(members)} entries, {total} uncompressed bytes", flush=True)
    root = destination.resolve()
    paths: set[str] = set()
    for member in members:
        name = member.filename.replace("\\", "/")
        relative = PurePosixPath(name)
        mode = member.external_attr >> 16
        file_type = stat.S_IFMT(mode)
        normalized = unicodedata.normalize("NFC", relative.as_posix().rstrip("/")).casefold()
        if (
            not name
            or "\0" in name
            or relative.is_absolute()
            or ".." in relative.parts
            or (relative.parts and ":" in relative.parts[0])
            or stat.S_ISLNK(mode)
            or file_type not in (0, stat.S_IFREG, stat.S_IFDIR)
            or normalized in paths
        ):
            raise ValueError(f"unsafe ZIP entry: {member.filename}")
        paths.add(normalized)

        target = root.joinpath(*relative.parts).resolve()
        try:
            target.relative_to(root)
        except ValueError as exc:
            raise ValueError(
                f"ZIP entry escapes the destination: {member.filename}"
            ) from exc
        if member.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        print(f"Extract: {member.filename} ({member.file_size} bytes)", flush=True)
        target.parent.mkdir(parents=True, exist_ok=True)
        with archive.open(member) as source, target.open("wb") as output:
            shutil.copyfileobj(source, output, length=1024 * 1024)
        if target.stat().st_size != member.file_size:
            raise ValueError(f"ZIP entry has the wrong size: {member.filename}")


def official_apk_assets(source: Path) -> tuple[bytes, bytes]:
    """The engine and launcher icon from the verified official APK."""
    candidates = sorted(source.rglob("*.apk"))
    if not candidates:
        raise ValueError(f"no APK found in source: {source}")

    seen: list[str] = []
    if len(candidates) > 100:
        raise ValueError(f"too many APK files in source: {len(candidates)}")
    for apk in candidates:
        print(f"Verify APK: {apk.name}", flush=True)
        with zipfile.ZipFile(apk) as archive:
            try:
                info = archive.getinfo(ENGINE_IN_APK)
            except KeyError:
                continue
            if info.file_size > MAX_ENGINE_BYTES:
                continue
            engine = archive.read(info)
            digest = sha256(engine)
            if digest != OFFICIAL_ENGINE_SHA256:
                seen.append(f"{apk.name}: {digest}")
                continue
            try:
                icon_info = archive.getinfo(APP_ICON_IN_APK)
            except KeyError as exc:
                raise ValueError(f"official APK lacks {APP_ICON_IN_APK}") from exc
            if icon_info.file_size > MAX_APP_ICON_BYTES:
                raise ValueError("official APK launcher icon is unexpectedly large")
            app_icon = archive.read(icon_info)
        icon_digest = sha256(app_icon)
        if icon_digest != OFFICIAL_APP_ICON_SHA256:
            raise ValueError(
                f"official APK launcher icon has the wrong digest: {icon_digest}"
            )
        print(f"official application assets: {apk.name}")
        return engine, app_icon

    raise ValueError(
        "no APK contains the official 1.8.1g engine; found engines:\n  "
        + "\n  ".join(seen)
    )


def game_data(source: Path) -> Path:
    """The `<package>` directory of the extracted application data."""
    matches = sorted(p for p in source.rglob(PACKAGE) if (p / "files").is_dir())
    if len(matches) != 1:
        raise ValueError(
            f"source must contain exactly one {PACKAGE}/files directory; "
            f"found: {len(matches)}"
        )
    return matches[0]


def copy_game_data(data: Path, target: Path) -> int:
    copied = 0
    for path in content_files(data):
        if path.name in NOT_INSTALLED:
            continue
        destination = target / path.relative_to(data)
        destination.parent.mkdir(parents=True, exist_ok=True)
        print(f"Copy game data: {path.relative_to(data)}", flush=True)
        shutil.copyfile(path, destination)
        copied += 1
    return copied


def populate_assets(source: Path, root: Path, assets: Path) -> None:
    engine, app_icon = official_apk_assets(source)
    data = game_data(source)

    (assets / "lib").mkdir(parents=True, exist_ok=True)
    (assets / "game").mkdir(parents=True, exist_ok=True)

    (assets / "app-icon.png").write_bytes(app_icon)
    (assets / "lib" / "libdespicablemefree.so").write_bytes(engine)
    files = copy_game_data(data, assets / "game")
    profiles_path = assets / "game/files/profiles.json"
    config = load_json_object(root / "config/graphics.json")
    profiles = load_json_object(profiles_path, ENCODING)
    write_profiles(profiles_path, apply_configuration(config, profiles))
    total = sum(path.stat().st_size for path in content_files(assets))
    print(f"assets/: 1 app icon + 1 engine + {files} game files, {total} bytes")


def replace_assets(staged: Path, destination: Path, backup: Path) -> None:
    """Commit a validated directory, restoring the previous tree on rename failure."""
    try:
        if destination.exists():
            destination.rename(backup)
        staged.rename(destination)
    except BaseException:
        if backup.exists() and not destination.exists():
            backup.rename(destination)
        raise


def install_release(
    source: Path, root: Path, destination: Path | None = None
) -> AssetReport:
    root = root.resolve()
    assets = destination if destination is not None else root / "assets"
    if (
        not root.is_dir() or root == Path(root.anchor) or assets.is_symlink()
        or assets == Path(assets.anchor) or not assets.parent.is_dir()
        or assets.resolve() == root or assets.resolve() in root.parents
        or (assets.exists() and not assets.is_dir())
    ):
        raise ValueError(f"unsafe project root: {root}")
    if not source.exists():
        raise ValueError(f"source does not exist: {source}")
    with tempfile.TemporaryDirectory(
        prefix="minion-rush-assets.", dir=assets.parent
    ) as temporary:
        staging = Path(temporary)
        extracted = staging / "release"
        if source.is_file():
            extracted.mkdir()
            with zipfile.ZipFile(source) as archive:
                extract_release_archive(archive, extracted)
        else:
            extracted = source
        print("Preparing release assets", flush=True)
        staged_assets = staging / "assets"
        populate_assets(extracted, root, staged_assets)
        print("Validating staged assets", flush=True)
        report = inspect_assets(root, staged_assets)
        print(report.summary(), flush=True)
        print("Committing validated assets", flush=True)
        replace_assets(staged_assets, assets, staging / "previous-assets")
        print("Asset import complete", flush=True)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="release ZIP or an extracted copy")
    parser.add_argument("--root", default=".", help="project root")
    args = parser.parse_args()

    source = Path(args.source).resolve()
    root = Path(args.root).resolve()
    try:
        report = install_release(source, root)
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as exc:
        print(f"asset installation failed: {exc}", file=sys.stderr)
        return 1
    print(report.summary())
    return 0


if __name__ == "__main__":
    sys.exit(main())
