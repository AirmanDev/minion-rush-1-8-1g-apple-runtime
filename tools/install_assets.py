#!/usr/bin/env python3
"""Rebuild assets/ from the official, unmodified Minion Rush 1.8.1g release.

The source is the distribution archive (or an already extracted copy of it)
that contains the signed APKs and one `data/com.gameloft.android.ANMP.GloftDMHM`
directory.  Several APKs in that archive carry patched engines; the official one
is picked by the SHA-256 of its ARM32 engine, so a modified build can never be
installed by accident.

`assets/` is removed and written from scratch: there is no merge step and no
state carried over from a previous install.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import stat
import sys
import tempfile
import zipfile
from pathlib import Path
from pathlib import PurePosixPath

sys.dont_write_bytecode = True

from common import content_files, load_json_object
from configure_graphics import ENCODING, apply_configuration, write_profiles

PACKAGE = "com.gameloft.android.ANMP.GloftDMHM"
ENGINE_IN_APK = "lib/armeabi-v7a/libdespicablemefree.so"
APP_ICON_IN_APK = "res/drawable-xxxhdpi/icon.png"

# The engine shipped in "1.8.1g Unmodded.apk". The costume/tourist builds are
# byte-identical apart from nine patch sites, so only the digest tells them
# apart. tools/validate_assets.py checks the installed copy against this value.
OFFICIAL_ENGINE_SHA256 = (
    "86c019f800ed6420a8991efc51a651a7347e02f54034df68c8df174e6340f082"
)
OFFICIAL_APP_ICON_SHA256 = (
    "85cb7bfbe3e49af96be8b4b4e54ebd94a0dd4309f130d2abd2d0ed135fc93ec5"
)

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

    root = destination.resolve()
    paths: set[str] = set()
    for member in members:
        name = member.filename.replace("\\", "/")
        relative = PurePosixPath(name)
        mode = member.external_attr >> 16
        file_type = stat.S_IFMT(mode)
        normalized = relative.as_posix().rstrip("/").casefold()
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
        target.parent.mkdir(parents=True, exist_ok=True)
        with archive.open(member) as source, target.open("wb") as output:
            shutil.copyfileobj(source, output, length=1024 * 1024)
        if target.stat().st_size != member.file_size:
            raise ValueError(f"ZIP entry has the wrong size: {member.filename}")


def official_apk_assets(source: Path) -> tuple[bytes, bytes]:
    """The engine and launcher icon from the verified official APK."""
    candidates = sorted(source.rglob("*.apk"))
    if not candidates:
        raise SystemExit(f"no APK found in source: {source}")

    seen: list[str] = []
    if len(candidates) > 100:
        raise SystemExit(f"too many APK files in source: {len(candidates)}")
    for apk in candidates:
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
                raise SystemExit(f"official APK lacks {APP_ICON_IN_APK}") from exc
            if icon_info.file_size > MAX_APP_ICON_BYTES:
                raise SystemExit("official APK launcher icon is unexpectedly large")
            app_icon = archive.read(icon_info)
        icon_digest = sha256(app_icon)
        if icon_digest != OFFICIAL_APP_ICON_SHA256:
            raise SystemExit(
                f"official APK launcher icon has the wrong digest: {icon_digest}"
            )
        print(f"official application assets: {apk.name}")
        return engine, app_icon

    raise SystemExit(
        "no APK contains the official 1.8.1g engine; found engines:\n  "
        + "\n  ".join(seen)
    )


def game_data(source: Path) -> Path:
    """The `<package>` directory of the extracted application data."""
    matches = sorted(p for p in source.rglob(PACKAGE) if (p / "files").is_dir())
    if len(matches) != 1:
        raise SystemExit(
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
        shutil.copyfile(path, destination)
        copied += 1
    return copied


def install(source: Path, root: Path) -> None:
    engine, app_icon = official_apk_assets(source)
    data = game_data(source)

    assets = root / "assets"
    if root == Path(root.anchor) or assets.is_symlink():
        raise SystemExit(f"unsafe project root: {root}")
    shutil.rmtree(assets, ignore_errors=True)
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="release ZIP or an extracted copy")
    parser.add_argument("--root", default=".", help="project root")
    args = parser.parse_args()

    source = Path(args.source).resolve()
    root = Path(args.root).resolve()
    if not source.exists():
        raise SystemExit(f"source does not exist: {source}")

    if source.is_file():
        with tempfile.TemporaryDirectory() as tmp:
            try:
                with zipfile.ZipFile(source) as archive:
                    extract_release_archive(archive, Path(tmp))
            except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as exc:
                raise SystemExit(f"invalid release ZIP: {exc}") from exc
            install(Path(tmp), root)
    else:
        install(source, root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
