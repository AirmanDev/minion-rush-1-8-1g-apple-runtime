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
from third_party_notices import COMMON_NOTICES, TOOLCHAIN_NOTICES
from package_portable_builders import package, verify_generic_template, verify_windows
from test_native_ipa import macho


def generic_template() -> bytes:
    payload = io.BytesIO()
    binary = macho() + b"game-profile.bin\0native ARM64 build profile missing or invalid\0"
    manifest = {
        "app_name": "MinionRush.app", "version": 3, "profile_version": 3,
        "slot_offset": 4096, "slot_size": 256,
        "executable_sha256": hashlib.sha256(binary).hexdigest(),
    }
    with zipfile.ZipFile(payload, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("template-manifest.json", json.dumps(manifest))
        archive.writestr("Template/MinionRush.app/MinionRush", binary)
    return payload.getvalue()


def synthetic_windows(path: Path, names: list[str]) -> None:
    packed = io.BytesIO()
    with zipfile.ZipFile(packed, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in names:
            archive.writestr(name, generic_template() if name.endswith(
                "MinionRush-native-template-unsigned.zip") else
                (b"synthetic Python license" * 80 if name.endswith("/python/LICENSE.txt")
                 else (ROOT / "third_party_licenses/mingw-w64-runtime-COPYING.txt").read_bytes()
                 if name.endswith("/mingw-w64-runtime-COPYING.txt") else
                 (ROOT / "third_party_licenses" / name.rsplit("/", 1)[-1]).read_bytes()
                 if name.rsplit("/", 1)[-1] in (COMMON_NOTICES | TOOLCHAIN_NOTICES) else
                 (ROOT / "language_packs/hu.json").read_bytes()
                 if name.endswith("/language_packs/hu.json") else
                 b"synthetic non-game data"))
    payload = packed.getvalue()
    launcher = b"MZ" + b"\0" * 62
    path.write_bytes(launcher + payload + FOOTER.pack(
        BUNDLE_MAGIC, len(launcher), len(payload),
        hashlib.sha256(payload).digest()
    ))


NAMES = [
    "MinionRush-Portable-Windows/MinionRush-native-template-unsigned.zip",
    "MinionRush-Portable-Windows/python/python314.zip",
    "MinionRush-Portable-Windows/python/LICENSE.txt",
    "MinionRush-Portable-Windows/runtime/LICENSE",
    "MinionRush-Portable-Windows/runtime/language_packs/hu.json",
    "MinionRush-Portable-Windows/runtime/third_party_licenses/mingw-w64-runtime-COPYING.txt",
] + ["MinionRush-Portable-Windows/runtime/third_party_licenses/" + name
     for name in (COMMON_NOTICES | TOOLCHAIN_NOTICES)]


class PortableBuilderPackagingTests(unittest.TestCase):
    def test_verifies_embedded_windows_kit(self):
        with tempfile.TemporaryDirectory() as location:
            executable = Path(location) / "Builder.exe"
            synthetic_windows(executable, NAMES)
            verify_windows(executable)

    def test_rejects_missing_or_changed_hungarian_pack(self):
        with tempfile.TemporaryDirectory() as location:
            executable = Path(location) / "Builder.exe"
            synthetic_windows(executable, [
                name for name in NAMES if not name.endswith("/language_packs/hu.json")
            ])
            with self.assertRaisesRegex(ValueError, "public language pack"):
                verify_windows(executable)

    def test_rejects_missing_python_component_notice(self):
        missing = next(iter(COMMON_NOTICES))
        with tempfile.TemporaryDirectory() as location:
            executable = Path(location) / "Builder.exe"
            synthetic_windows(executable, [
                name for name in NAMES if not name.endswith("/" + missing)
            ])
            with self.assertRaisesRegex(ValueError, "license notice"):
                verify_windows(executable)

    def test_rejects_missing_toolchain_notice(self):
        missing = next(iter(TOOLCHAIN_NOTICES))
        with tempfile.TemporaryDirectory() as location:
            executable = Path(location) / "Builder.exe"
            synthetic_windows(executable, [
                name for name in NAMES if not name.endswith("/" + missing)
            ])
            with self.assertRaisesRegex(ValueError, "license notice"):
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

    def test_template_with_missing_profile_loader_is_rejected(self):
        with zipfile.ZipFile(io.BytesIO(generic_template())) as source:
            info = json.loads(source.read("template-manifest.json"))
            executable = source.read("Template/MinionRush.app/MinionRush")
        missing_loader = executable.replace(b"game-profile.bin", b"game-profile.old")
        info["executable_sha256"] = hashlib.sha256(missing_loader).hexdigest()
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, "w", zipfile.ZIP_DEFLATED) as dest:
            dest.writestr("template-manifest.json", json.dumps(info))
            dest.writestr("Template/MinionRush.app/MinionRush", missing_loader)
        with self.assertRaisesRegex(ValueError, "profile"):
            verify_generic_template(stream.getvalue())

    def test_rejects_missing_python_and_project_licenses(self):
        for removed in ("/python/LICENSE.txt", "/runtime/LICENSE"):
            with self.subTest(removed=removed), tempfile.TemporaryDirectory() as folder:
                exe = Path(folder) / "Builder.exe"
                synthetic_windows(exe, [item for item in NAMES if not item.endswith(removed)])
                with self.assertRaisesRegex(ValueError, "license"):
                    verify_windows(exe)

    def test_rejects_missing_runtime_attribution(self):
        with tempfile.TemporaryDirectory() as location:
            executable = Path(location) / "Builder.exe"
            synthetic_windows(executable, [x for x in NAMES
                                           if not x.endswith("/mingw-w64-runtime-COPYING.txt")])
            with self.assertRaisesRegex(ValueError, "runtime attribution"):
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
