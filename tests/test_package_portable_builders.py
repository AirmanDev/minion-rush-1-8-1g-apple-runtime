"""Check the portable, generic game-data-free release bundle contract."""

from __future__ import annotations

import hashlib
import io
import json
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from build_windows_onefile import BUNDLE_MAGIC, FOOTER
from package_portable_builders import package, verify_generic_template, verify_windows
from test_native_ipa import macho


def generic_template() -> bytes:
    payload = io.BytesIO()
    manifest = {
        "app_name": "MinionRush.app", "version": 3, "profile_version": 3,
        "slot_offset": 4096, "slot_size": 256,
        "executable_sha256": hashlib.sha256(macho()).hexdigest(),
    }
    with zipfile.ZipFile(payload, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("template-manifest.json", json.dumps(manifest))
        archive.writestr("Template/MinionRush.app/MinionRush", macho())
    return payload.getvalue()


def synthetic_windows(path: Path, names: list[str]) -> None:
    packed = io.BytesIO()
    with zipfile.ZipFile(packed, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in names:
            archive.writestr(name, generic_template() if name.endswith(
                "MinionRush-native-template-unsigned.zip") else b"synthetic non-game data")
    payload = packed.getvalue()
    launcher = b"MZ" + b"\0" * 62
    path.write_bytes(launcher + payload + FOOTER.pack(
        BUNDLE_MAGIC, len(launcher), len(payload),
        hashlib.sha256(payload).digest()
    ))


NAMES = [
    "MinionRush-Portable-Windows/MinionRush-native-template-unsigned.zip",
    "MinionRush-Portable-Windows/python/python314.zip",
]


class PortableBuilderPackagingTests(unittest.TestCase):
    def test_verifies_embedded_windows_kit(self):
        with tempfile.TemporaryDirectory() as location:
            executable = Path(location) / "Builder.exe"
            synthetic_windows(executable, NAMES)
            verify_windows(executable)

    def test_template_is_independently_verified_and_empty(self):
        verify_generic_template(generic_template())
        with zipfile.ZipFile(io.BytesIO(generic_template())) as archive:
            executable = bytearray(archive.read("Template/MinionRush.app/MinionRush"))
        executable[4096] = 1
        modified = io.BytesIO()
        manifest = {
            "app_name": "MinionRush.app", "version": 3,
            "profile_version": 3, "slot_offset": 4096,
            "slot_size": 256,
            "executable_sha256": hashlib.sha256(executable).hexdigest(),
        }
        with zipfile.ZipFile(modified, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("template-manifest.json", json.dumps(manifest))
            archive.writestr("Template/MinionRush.app/MinionRush", executable)
        with self.assertRaisesRegex(ValueError, "nonzero"):
            verify_generic_template(modified.getvalue())

    def test_rejects_modified_embedded_kit(self):
        with tempfile.TemporaryDirectory() as location:
            executable = Path(location) / "Builder.exe"
            synthetic_windows(executable, NAMES)
            data = bytearray(executable.read_bytes())
            data[100] ^= 1
            executable.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "checksum"):
                verify_windows(executable)

    def test_refuses_ipa_and_original_game_packages(self):
        with tempfile.TemporaryDirectory() as location:
            executable = Path(location) / "Builder.exe"
            synthetic_windows(executable, NAMES + [
                "MinionRush-Portable-Windows/original.ipa",
            ])
            with self.assertRaisesRegex(ValueError, "game content"):
                verify_windows(executable)

    def test_rejects_private_hungarian_and_missing_template(self):
        with tempfile.TemporaryDirectory() as location:
            executable = Path(location) / "Builder.exe"
            synthetic_windows(executable, NAMES + [
                "MinionRush-Portable-Windows/private-localizations/hu.xlf",
            ])
            with self.assertRaisesRegex(ValueError, "private language"):
                verify_windows(executable)
            synthetic_windows(executable, NAMES[1:])
            with self.assertRaisesRegex(ValueError, "missing"):
                verify_windows(executable)

    def test_packager_rejects_output_outside_private_build_root(self):
        with tempfile.TemporaryDirectory() as location:
            root = Path(location)
            with self.assertRaisesRegex(ValueError, "under build/portable-releases"):
                package(root / "missing.app", root / "missing.exe",
                        root / "1.1.0", "1.1.0")


if __name__ == "__main__":
    unittest.main()
