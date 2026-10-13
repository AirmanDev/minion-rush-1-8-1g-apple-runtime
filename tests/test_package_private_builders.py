"""Check the offline-only private release bundle contract."""

from __future__ import annotations

import hashlib
import io
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from build_windows_onefile import BUNDLE_MAGIC, FOOTER
from package_private_builders import package, verify_windows


def synthetic_windows(path: Path, names: list[str]) -> None:
    packed = io.BytesIO()
    with zipfile.ZipFile(packed, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in names:
            archive.writestr(name, b"synthetic non-game data")
    payload = packed.getvalue()
    launcher = b"MZ" + b"\0" * 62
    path.write_bytes(launcher + payload + FOOTER.pack(
        BUNDLE_MAGIC, len(launcher), len(payload),
        hashlib.sha256(payload).digest()
    ))


NAMES = [
    "MinionRush-Portable-Windows/private-localizations/hu.xlf",
    "MinionRush-Portable-Windows/MinionRush-native-template-unsigned.zip",
    "MinionRush-Portable-Windows/python/python314.zip",
]


class PrivateBuilderPackagingTests(unittest.TestCase):
    def test_verifies_embedded_windows_kit(self):
        with tempfile.TemporaryDirectory() as location:
            executable = Path(location) / "Builder.exe"
            synthetic_windows(executable, NAMES)
            verify_windows(executable)

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

    def test_requires_local_hungarian_and_template(self):
        with tempfile.TemporaryDirectory() as location:
            executable = Path(location) / "Builder.exe"
            for removed in NAMES[:2]:
                synthetic_windows(executable, [n for n in NAMES if n != removed])
                with self.assertRaisesRegex(ValueError, "missing"):
                    verify_windows(executable)

    def test_packager_rejects_output_outside_private_build_root(self):
        with tempfile.TemporaryDirectory() as location:
            root = Path(location)
            with self.assertRaisesRegex(ValueError, "under build/private-releases"):
                package(root / "missing.app", root / "missing.exe",
                        root / "1.1.0", "1.1.0")


if __name__ == "__main__":
    unittest.main()
