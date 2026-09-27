from __future__ import annotations

import stat
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
sys.dont_write_bytecode = True

from ipa_archive import export_ipa


class IPAArchiveTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.archive = self.directory / "game.ipa"
        self.application = self.directory / "MinionRush.app"
        self.application.mkdir()
        self.binary = self.application / "MinionRush"
        self.binary.write_bytes(b"signed bytes")
        self.binary.chmod(0o755)

    def assert_no_temporary_archives(self) -> None:
        self.assertFalse(list(self.directory.glob(".minion-rush-*")))

    def test_export_preserves_bytes_permissions_and_bundle_layout(self) -> None:
        resources = self.application / "Resources"
        resources.mkdir()
        (resources / "data.bin").write_bytes(b"private fixture")
        export_ipa(self.application, self.archive)
        with zipfile.ZipFile(self.archive) as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(set(archive.namelist()), {
                "Payload/MinionRush.app/MinionRush",
                "Payload/MinionRush.app/Resources/data.bin",
            })
            self.assertEqual(archive.read("Payload/MinionRush.app/MinionRush"), b"signed bytes")
            self.assertEqual(archive.read("Payload/MinionRush.app/Resources/data.bin"), b"private fixture")
            mode = archive.getinfo("Payload/MinionRush.app/MinionRush").external_attr >> 16
            self.assertEqual(stat.S_IMODE(mode), 0o755)
        self.assert_no_temporary_archives()

    def test_replaces_an_existing_archive_on_success(self) -> None:
        self.archive.write_bytes(b"previous archive")
        export_ipa(self.application, self.archive)
        self.assertTrue(zipfile.is_zipfile(self.archive))
        self.assert_no_temporary_archives()

    def test_accepts_uppercase_ipa_extension(self) -> None:
        destination = self.directory / "game.IPA"
        export_ipa(self.application, destination)
        self.assertTrue(zipfile.is_zipfile(destination))

    def test_rejects_invalid_destination_or_bundle(self) -> None:
        renamed = self.directory / "NotAnApp"
        renamed.mkdir()
        linked = self.directory / "Linked.app"
        linked.symlink_to(self.application, target_is_directory=True)
        for application, destination in (
            (self.application, self.directory / "game.zip"),
            (self.directory / "Missing.app", self.archive),
            (self.binary, self.archive),
            (renamed, self.archive),
            (linked, self.archive),
        ):
            with self.subTest(application=application, destination=destination):
                with self.assertRaises(ValueError):
                    export_ipa(application, destination)
        self.assertFalse(self.archive.exists())
        self.assert_no_temporary_archives()

    def test_rejects_links_and_preserves_previous_archive(self) -> None:
        self.archive.write_bytes(b"previous archive")
        linked = self.application / "linked"
        for target in (self.binary, self.directory / "missing", self.application):
            with self.subTest(target=target):
                linked.symlink_to(target)
                try:
                    with self.assertRaisesRegex(ValueError, "Linked"):
                        export_ipa(self.application, self.archive)
                finally:
                    linked.unlink()
                self.assertEqual(self.archive.read_bytes(), b"previous archive")
                self.assert_no_temporary_archives()

    def test_failed_archive_write_preserves_previous_file(self) -> None:
        self.archive.write_bytes(b"previous archive")
        with mock.patch("ipa_archive.zipfile.ZipFile.write", side_effect=OSError("write failed")):
            with self.assertRaisesRegex(OSError, "write failed"):
                export_ipa(self.application, self.archive)
        self.assertEqual(self.archive.read_bytes(), b"previous archive")
        self.assert_no_temporary_archives()

    def test_failed_commit_preserves_previous_file(self) -> None:
        self.archive.write_bytes(b"previous archive")
        with mock.patch("ipa_archive.os.replace", side_effect=OSError("replace failed")):
            with self.assertRaisesRegex(OSError, "replace failed"):
                export_ipa(self.application, self.archive)
        self.assertEqual(self.archive.read_bytes(), b"previous archive")
        self.assert_no_temporary_archives()


if __name__ == "__main__":
    unittest.main()
