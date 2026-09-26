#!/usr/bin/env python3
"""Read-only validation for the shipped engine and game assets."""

from __future__ import annotations

import argparse
import hashlib
import re
import sqlite3
import struct
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

sys.dont_write_bytecode = True

from common import (  # noqa: E402
    content_files,
    is_disposable,
    load_json,
    load_json_object,
    walk_files,
)
from configure_graphics import validate as validate_graphics
from asset_contract import OFFICIAL_APP_ICON_SHA256, OFFICIAL_ENGINE_SHA256

ENGINE = "lib/libdespicablemefree.so"
APP_ICON = "app-icon.png"
ASSET_REQUIRED = (
    APP_ICON,
    ENGINE,
    "game/PN.db",
    "game/files/profiles.json",
    "game/files/events.json",
    "game/files/d_o_w_n_l_o_a_d_e_d.txt",
    "game/files/dlcs/mnhtn_index_android",
)
SOURCE_REQUIRED = ("config/graphics.json",)


def validate_engine(path: Path) -> None:
    """Refuse a patched engine: only the official 1.8.1g build may be installed."""
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != OFFICIAL_ENGINE_SHA256:
        raise ValueError(
            f"engine is not the official 1.8.1g release: {digest}; "
            "run tools/install_assets.py"
        )


def validate_app_icon(path: Path) -> None:
    """Require the unmodified launcher icon from the official 1.8.1g APK."""
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != OFFICIAL_APP_ICON_SHA256:
        raise ValueError(
            f"app icon is not from the official 1.8.1g release: {digest}; "
            "run tools/install_assets.py"
        )
    if len(data) < 26 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("app icon is not a PNG")
    width, height, depth, color = struct.unpack(">IIBB", data[16:26])
    if (width, height, depth, color) != (576, 576, 8, 6):
        raise ValueError("app icon has unexpected PNG properties")


def validate_database(path: Path) -> None:
    uri = f"file:{path.resolve()}?mode=ro&immutable=1"
    with sqlite3.connect(uri, uri=True) as db:
        result = db.execute("PRAGMA quick_check").fetchone()
    if result != ("ok",):
        raise ValueError(f"SQLite quick_check failed: {path}: {result}")


def validate_jpk_archives(directory: Path) -> None:
    archives = sorted(directory.glob("*.jpk"))
    if not archives:
        raise ValueError(f"no JPK archive found: {directory}")

    for path in archives:
        try:
            with zipfile.ZipFile(path) as archive:
                if not archive.infolist():
                    raise ValueError(f"empty JPK archive: {path.name}")
                damaged = archive.testzip()
        except zipfile.BadZipFile as exc:
            raise ValueError(f"damaged or truncated JPK archive: {path.name}") from exc
        if damaged is not None:
            raise ValueError(f"damaged JPK entry: {path.name}: {damaged}")


def validate_single_graphics_profile(root: Path, assets: Path) -> None:
    full_config = load_json_object(root / "config/graphics.json")
    profiles = load_json_object(assets / "game/files/profiles.json")
    validate_graphics(full_config, profiles)

    obsolete_profile = assets / "game/files/prAfiles.json"
    if obsolete_profile.exists():
        raise ValueError("obsolete duplicate profile remains: game/files/prAfiles.json")


def validate_offline_event_catalog(root: Path, assets: Path) -> None:
    source = (root / "src/native/offline_events.c").read_text(encoding="utf-8")
    events = re.findall(
        r'\{\s*"(OnlineMission_[A-Za-z_]+)"\s*,\s*"(OM_[A-Z0-9_]+)"\s*,\s*'
        r'"(OM_[A-Z0-9_]+)"\s*,\s*\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}\s*\}',
        source,
    )
    if len(events) != 20 or len({event[0] for event in events}) != 20:
        raise ValueError(f"invalid offline weekly event catalog: {len(events)} entries")
    for event in events:
        tiers = [int(value) for value in event[3:]]
        if tiers != sorted(set(tiers)) or tiers[0] < 1:
            raise ValueError(f"weekly goal thresholds are not increasing: {event[0]} {tiers}")

    awards = re.findall(r'#define WEEKLY_GOAL_AWARD "(OnlineAward_[A-Za-z_0-9]+)"', source)
    if len(awards) != 1:
        raise ValueError("exactly one weekly goal award is required")

    bindings = (root / "src/native/game_symbols.def").read_text(encoding="utf-8")
    version_binding = (
        'MR_GAME_SYMBOL(EVENT_VERSION, "_ZN9EventsMgr14k_eventVersionE")'
    )
    if (
        r'\"_version\":\"%s\",' not in source
        or "MR_GAME_EVENT_VERSION" not in source
        or bindings.count(version_binding) != 1
    ):
        raise ValueError(
            "offline events do not use the verified build-time engine version binding"
        )

    engine = assets / "lib/libdespicablemefree.so"
    engine_data = engine.read_bytes()
    for token in (b"_version\0", b"_ZN9EventsMgr14k_eventVersionE\0"):
        if token not in engine_data:
            raise ValueError("engine lacks the event-version evidence")

    evidence_paths = (
        assets / "game/files/levels/introlib.blibclara",
        assets / "game/files/text/en.texts",
        assets / "game/files/dlcs/dlc_v2_online_regions_mandatory_android_onlineregions_33.jpk",
        assets / "game/files/dlcs/dlc_v2_data_mandatory_android_data_85.jpk",
    )
    # Mission names, text keys, and award types must exist in the original game
    # data. Thresholds and amounts are local calibrations because the retired
    # service supplied them and no authoritative copy remains. Search each file
    # independently so a token cannot cross a file boundary by accident.
    evidence = [path.read_bytes() for path in evidence_paths]
    missing = sorted(
        token
        for event in events
        for token in event[:3] + tuple(awards)
        if not any(token.encode("ascii") in data for data in evidence)
    )
    if missing:
        raise ValueError("missing from the original game data: " + ", ".join(missing))


def record(errors: list[str], check: Callable[..., object], *arguments: object) -> None:
    """Run one check and keep going, so a single report lists every fault."""
    try:
        check(*arguments)
    except (OSError, KeyError, ValueError, sqlite3.Error) as exc:
        errors.append(str(exc))


@dataclass(frozen=True)
class AssetReport:
    files: int
    bytes: int
    json_files: int

    def summary(self) -> str:
        return (
            f"asset validation: passed ({self.files} files, {self.bytes} bytes, "
            f"{self.json_files} JSON files, SQLite ok)"
        )


def inspect_assets(root: Path, assets: Path) -> AssetReport:

    disposable = [
        path.relative_to(assets)
        for path in walk_files(assets)
        if is_disposable(path.name)
    ] if assets.is_dir() else []
    files = list(content_files(assets))

    errors: list[str] = []
    if disposable:
        errors.append(
            "disposable metadata in asset tree: "
            + ", ".join(str(path) for path in disposable[:10])
        )
    for tree, required, label in ((root, SOURCE_REQUIRED, ""), (assets, ASSET_REQUIRED, " asset")):
        for relative in required:
            path = tree / relative
            if not path.is_file() or path.stat().st_size == 0:
                errors.append(f"missing or empty{label}: {relative}")

    json_files = [path for path in files if path.suffix == ".json"]
    for path in json_files:
        record(errors, load_json, path)
    record(errors, validate_engine, assets / ENGINE)
    record(errors, validate_app_icon, assets / APP_ICON)
    record(errors, validate_database, assets / "game/PN.db")
    record(errors, validate_jpk_archives, assets / "game/files/dlcs")
    record(errors, validate_single_graphics_profile, root, assets)
    record(errors, validate_offline_event_catalog, root, assets)

    if errors:
        raise ValueError("\n".join(errors))

    byte_count = sum(path.stat().st_size for path in files)
    return AssetReport(len(files), byte_count, len(json_files))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", default=".")
    parser.add_argument("--assets-root", default="assets")
    args = parser.parse_args()
    root = Path(args.root).resolve()
    assets = Path(args.assets_root)
    assets = assets.resolve() if assets.is_absolute() else (root / assets).resolve()
    try:
        report = inspect_assets(root, assets)
    except ValueError as exc:
        print(f"asset validation: FAILED\n{exc}", file=sys.stderr)
        return 1
    print(report.summary())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
