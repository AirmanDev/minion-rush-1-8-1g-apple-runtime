#!/usr/bin/env python3
"""Sign a locally assembled native IPA with an existing iOS provisioning profile."""

from __future__ import annotations

import datetime
import hashlib
import os
import plistlib
import re
import shutil
import stat
import tempfile
import zipfile
from pathlib import Path


def select_profile(directory: Path, team: str, bundle: str, udid: str,
                   identities: set[str], decode) -> tuple[Path, dict, str]:
    matches = []
    for file in sorted(directory.glob("*.mobileprovision"))[:100]:
        if file.is_symlink() or file.stat().st_size > 4 * 1024 * 1024:
            continue
        try:
            profile = plistlib.loads(decode(file))
        except (OSError, ValueError, plistlib.InvalidFileException):
            continue
        entitlements = profile.get("Entitlements", {})
        prefix = team + "."
        app_id = entitlements.get("application-identifier", "")
        if (team not in profile.get("TeamIdentifier", [])
                or not isinstance(app_id, str) or not app_id.startswith(prefix)
                or "iOS" not in profile.get("Platform", [])
                or udid not in profile.get("ProvisionedDevices", [])
                or profile.get("ExpirationDate", datetime.datetime.min)
                    <= datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)):
            continue
        suffix = app_id[len(prefix):]
        if suffix not in (bundle, "*"):
            continue
        certificate = next((
            hashlib.sha1(cert).hexdigest().upper()
            for cert in profile.get("DeveloperCertificates", [])
            if hashlib.sha1(cert).hexdigest().upper() in identities
        ), None)
        if certificate is None:
            continue
        matches.append((suffix == bundle, profile["ExpirationDate"], file,
                        entitlements, certificate))
    if not matches:
        raise ValueError(
            "No valid signing profile covers this device and bundle ID. "
            "In Xcode, enable automatic signing with this Team and connect "
            "the device once, then refresh and retry."
        )
    _, _, file, entitlements, certificate = max(matches, key=lambda item: (item[0], item[1]))
    return file, entitlements, certificate


def unpack_native_app(ipa: Path, destination: Path) -> Path:
    app = destination / "Payload/MinionRush.app"
    names = set()
    with zipfile.ZipFile(ipa) as source:
        files = source.infolist()
        if not files or len(files) > 12000 or sum(x.file_size for x in files) > 3 * 1024**3:
            raise ValueError("Invalid generated IPA size or entry count")
        for entry in files:
            name = entry.filename
            parts = name.split("/")
            mode = entry.external_attr >> 16
            if (entry.is_dir() or len(parts) < 3 or parts[:2] != ["Payload", "MinionRush.app"]
                    or any(part in ("", ".", "..") for part in parts)
                    or "\\" in name or not mode or
                    stat.S_IFMT(mode) not in (0, stat.S_IFREG)):
                raise ValueError("Invalid generated IPA member")
            key = name.casefold()
            if key in names:
                raise ValueError("Duplicate generated IPA member")
            names.add(key)
        if not {"payload/minionrush.app/info.plist",
                "payload/minionrush.app/minionrush"}.issubset(names):
            raise ValueError("Generated IPA application is incomplete")
        for entry in files:
            source.extract(entry, destination)
    (app / "MinionRush").chmod(0o755)
    return app


def signing_material(device: dict[str, str], team: str, bundle: str,
                     environment: dict[str, str], run) -> tuple[Path, dict, str]:
    identities = run(["/usr/bin/security", "find-identity", "-v", "-p", "codesigning"],
                     environment)
    valid = set(re.findall(r"\b[A-F0-9]{40}\b", identities.upper()))
    directory = Path.home() / "Library/Developer/Xcode/UserData/Provisioning Profiles"
    return select_profile(
        directory, team, bundle, device["udid"], valid,
        lambda file: run(["/usr/bin/security", "cms", "-D", "-i", str(file)],
                         environment).encode(),
    )


def sign_and_install(ipa: Path, device: dict[str, str], team: str, bundle: str,
                     environment: dict[str, str], run, emit,
                     material: tuple[Path, dict, str]) -> None:
    profile, entitlements, certificate = material
    with tempfile.TemporaryDirectory(prefix="minion-rush-sign-") as temp:
        root = Path(temp)
        app = unpack_native_app(ipa, root)
        with (app / "Info.plist").open("rb") as file:
            info = plistlib.load(file)
        if info.get("CFBundleExecutable") != "MinionRush":
            raise ValueError("Unexpected native game executable")
        info["CFBundleIdentifier"] = bundle
        with (app / "Info.plist").open("wb") as file:
            plistlib.dump(info, file)
        shutil.copy2(profile, app / "embedded.mobileprovision")
        entitlements = dict(entitlements)
        entitlements["application-identifier"] = team + "." + bundle
        if "com.apple.developer.team-identifier" in entitlements:
            entitlements["com.apple.developer.team-identifier"] = team
        if "keychain-access-groups" in entitlements:
            entitlements["keychain-access-groups"] = [
                group.replace(team + ".*", team + "." + bundle)
                for group in entitlements["keychain-access-groups"]
            ]
        signed_entitlements = root / "entitlements.plist"
        with signed_entitlements.open("wb") as file:
            plistlib.dump(entitlements, file)
        emit("progress", message="Signing the native game")
        run(["/usr/bin/codesign", "--force", "--sign", certificate,
             "--timestamp=none", "--entitlements", str(signed_entitlements),
             "--generate-entitlement-der", str(app)], environment, timeout=120)
        run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)],
            environment, timeout=60)
        emit("progress", message="Installing on the selected device")
        run(["/usr/bin/xcrun", "devicectl", "device", "install", "app",
             "--device", device["id"], str(app)], environment, timeout=180)
        emit("log", message="Signed native game installed on the selected device.")
