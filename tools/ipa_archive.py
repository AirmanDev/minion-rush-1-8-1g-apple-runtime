"""Bounded IPA preflight shared by export and device installation."""

from __future__ import annotations

import os
import plistlib
import re
import stat
import tempfile
import zipfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Callable

from signing import signing_settings


@dataclass(frozen=True)
class IPAReport:
    bundle: str
    team: str
    expires: str
    devices: list[str]
    minimum_os: str
    bytes: int


def inspect_ipa(path: Path, decode_profile: Callable[[bytes], dict],
                *, rules_path: Path | None = None) -> IPAReport:
    if path.suffix.casefold() != ".ipa" or not path.is_file():
        raise ValueError("Choose a signed IPA exported by the Mac installer.")
    if path.stat().st_size > 4 * 1024**3:
        raise ValueError("The IPA exceeds the 4 GB size limit.")
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        if not entries or len(entries) > 30000:
            raise ValueError("Invalid IPA entry count.")
        names: set[str] = set()
        total = 0
        roots: set[str] = set()
        for entry in entries:
            parts = PurePosixPath(entry.filename).parts
            mode = entry.external_attr >> 16
            if (not parts or entry.filename.startswith("/") or ".." in parts
                    or "\\" in entry.filename or "\x00" in entry.filename
                    or entry.filename in names or entry.flag_bits & 1
                    or stat.S_ISLNK(mode)):
                raise ValueError("Unsafe, duplicate, linked, or encrypted IPA entry.")
            if parts[0] != "Payload" or (len(parts) > 1 and not parts[1].endswith(".app")):
                raise ValueError("The IPA must contain one application under Payload.")
            if len(parts) > 1:
                roots.add(parts[1])
            names.add(entry.filename)
            total += entry.file_size
            if entry.file_size > 1024**3 or total > 4 * 1024**3:
                raise ValueError("Uncompressed IPA exceeds its size limit.")
        if len(roots) != 1:
            raise ValueError("The IPA must contain exactly one application.")
        root = "Payload/" + roots.pop() + "/"

        def read_small(name: str) -> bytes:
            try:
                entry = archive.getinfo(root + name)
            except KeyError as exc:
                raise ValueError(f"Missing IPA file: {name}") from exc
            if entry.file_size > 4 * 1024**2:
                raise ValueError(f"Oversized IPA metadata: {name}")
            return archive.read(entry)

        info = plistlib.loads(read_small("Info.plist"))
        if not isinstance(info, dict):
            raise ValueError("Invalid application metadata.")
        if info.get("MRGuestVersion") != "1.8.1g":
            raise ValueError("The IPA is not this project's Minion Rush 1.8.1g runtime.")
        executable = info.get("CFBundleExecutable", "")
        if (not isinstance(executable, str) or not executable or "/" in executable
                or "\\" in executable or executable in (".", "..")
                or root + executable not in names
                or root + "_CodeSignature/CodeResources" not in names):
            raise ValueError("The application has no executable or code signature.")
        bundle = info.get("CFBundleIdentifier", "")
        if not isinstance(bundle, str):
            raise ValueError("Invalid bundle identifier.")
        profile = decode_profile(read_small("embedded.mobileprovision"))
        if not isinstance(profile, dict):
            raise ValueError("Invalid provisioning profile.")
        teams = profile.get("TeamIdentifier")
        devices = profile.get("ProvisionedDevices")
        expires = profile.get("ExpirationDate")
        entitlements = profile.get("Entitlements", {})
        if (not isinstance(teams, list) or len(teams) != 1
                or not isinstance(teams[0], str)
                or not isinstance(devices, list) or not devices
                or any(not isinstance(device, str) or not device for device in devices)
                or not isinstance(expires, datetime) or not isinstance(entitlements, dict)):
            raise ValueError("A device-bound development provisioning profile is required.")
        signing_settings(teams[0], bundle, rules_path=rules_path)
        app_id = entitlements.get("application-identifier")
        if app_id != teams[0] + "." + bundle:
            raise ValueError("The profile does not match the application identifier.")
        expiry = expires.replace(tzinfo=timezone.utc) if expires.tzinfo is None else expires
        if expiry <= datetime.now(timezone.utc):
            raise ValueError("The provisioning profile has expired. Export a new IPA on the Mac.")
        minimum = info.get("MinimumOSVersion", "17.0")
        if not isinstance(minimum, str) or not re.fullmatch(r"\d+(?:\.\d+){0,2}", minimum):
            raise ValueError("Invalid minimum operating system version.")
        return IPAReport(bundle, teams[0], expiry.isoformat(), devices, minimum, path.stat().st_size)


def export_ipa(application: Path, destination: Path) -> None:
    if destination.suffix.casefold() != ".ipa":
        raise ValueError("The exported archive must use the .ipa extension.")
    if not application.is_dir() or application.is_symlink() or application.suffix != ".app":
        raise ValueError("The signed application bundle is missing.")
    descriptor, temporary = tempfile.mkstemp(prefix=".minion-rush-", suffix=".ipa", dir=destination.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
                for source in sorted(application.rglob("*")):
                    if source.is_symlink():
                        raise ValueError("Linked files cannot be exported in the application.")
                    if source.is_file():
                        archive.write(source, "Payload/" + application.name + "/"
                                      + source.relative_to(application).as_posix())
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, destination)
    finally:
        Path(temporary).unlink(missing_ok=True)
