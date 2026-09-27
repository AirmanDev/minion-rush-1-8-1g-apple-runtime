from __future__ import annotations

import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
sys.dont_write_bytecode = True

from validate_windows_installer import validate


class WindowsPackageTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.package = Path(temporary.name)
        for source, destination in (("LICENSE", "LICENSE"), ("config/installer_ui.json", "installer_ui.json"),
                                    ("docs/INSTALLER_WINDOWS.md", "README.md")):
            shutil.copyfile(TOOLS.parent / source, self.package / destination)
        for name in ("THIRD_PARTY_NOTICES.txt", "DOTNET_LICENSE.txt", "DOTNET_NOTICES.txt"):
            (self.package / name).write_text("Fixture license notices")
        (self.package / "backend").mkdir()
        executable = bytearray(0x90)
        executable[:2] = b"MZ"
        struct.pack_into("<I", executable, 0x3C, 0x80)
        executable[0x80:0x84] = b"PE\x00\x00"
        struct.pack_into("<H", executable, 0x84, 0x8664)
        for name in ("MinionRushInstaller.exe", "backend/MinionRushDeviceBackend.exe"):
            (self.package / name).write_bytes(executable)

    def test_valid_public_package(self) -> None:
        validate(TOOLS.parent, self.package)

    def test_extra_private_and_linked_files_are_rejected(self) -> None:
        for name in ("backend/game.ipa", "backend/release.apk", "backend/profile.mobileprovision"):
            with self.subTest(name=name):
                entry = self.package / name
                entry.write_bytes(b"private")
                with self.assertRaisesRegex(ValueError, "Private"):
                    validate(TOOLS.parent, self.package)
                entry.unlink()
        (self.package / "stale.pdb").write_bytes(b"stale")
        with self.assertRaisesRegex(ValueError, "Unexpected"):
            validate(TOOLS.parent, self.package)

    def test_changed_contract_and_wrong_architecture_are_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "architecture"):
            validate(TOOLS.parent, self.package, 0xAA64)
        (self.package / "installer_ui.json").write_text("{}")
        with self.assertRaisesRegex(ValueError, "differs"):
            validate(TOOLS.parent, self.package)

    def test_missing_notices_are_rejected(self) -> None:
        (self.package / "DOTNET_LICENSE.txt").write_text("")
        with self.assertRaisesRegex(ValueError, "license"):
            validate(TOOLS.parent, self.package)


if __name__ == "__main__":
    unittest.main()
