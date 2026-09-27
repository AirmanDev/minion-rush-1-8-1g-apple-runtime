from __future__ import annotations

import copy
import plistlib
import stat
import sys
import tempfile
import unittest
import zipfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
sys.dont_write_bytecode = True

from ipa_archive import export_ipa, inspect_ipa
from ipa_device_backend import check_device


class IPAArchiveTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.archive = self.directory / "game.ipa"
        self.info = {"CFBundleIdentifier": "org.example.game", "CFBundleExecutable": "MinionRush",
                     "MRGuestVersion": "1.8.1g", "MinimumOSVersion": "17.0"}
        self.profile = {"TeamIdentifier": ["ABCDEFGHIJ"], "ProvisionedDevices": ["registered"],
                        "ExpirationDate": datetime.now(timezone.utc) + timedelta(days=1),
                        "Entitlements": {"application-identifier": "ABCDEFGHIJ.org.example.game"}}

    def write_archive(self, extra: dict[str, bytes] | None = None, omitted: str = "") -> None:
        files = {"Info.plist": plistlib.dumps(self.info), "MinionRush": b"fixture executable",
                 "embedded.mobileprovision": b"fixture CMS", "_CodeSignature/CodeResources": b"fixture signature"}
        with zipfile.ZipFile(self.archive, "w") as archive:
            for name, value in files.items():
                if name != omitted:
                    archive.writestr("Payload/MinionRush.app/" + name, value)
            for name, value in (extra or {}).items():
                archive.writestr(name, value)

    def inspect(self):
        return inspect_ipa(self.archive, lambda data: copy.deepcopy(self.profile))

    def test_valid_runtime_profile_and_exact_device(self) -> None:
        self.write_archive()
        report = self.inspect()
        self.assertEqual(report.bundle, self.info["CFBundleIdentifier"])
        self.assertEqual(report.team, "ABCDEFGHIJ")
        check_device(report, {"udid": "registered", "os": "17"})
        with self.assertRaisesRegex(ValueError, "not in"):
            check_device(report, {"udid": "other", "os": "27.0"})
        with self.assertRaisesRegex(ValueError, "older"):
            check_device(report, {"udid": "registered", "os": "16.6"})

    def test_higher_minimum_is_respected(self) -> None:
        self.info["MinimumOSVersion"] = "18.1"
        self.write_archive()
        with self.assertRaisesRegex(ValueError, "older"):
            check_device(self.inspect(), {"udid": "registered", "os": "18"})
        check_device(self.inspect(), {"udid": "registered", "os": "18.1"})

    def test_rejects_expired_mismatched_or_non_device_profile(self) -> None:
        self.write_archive()
        for key, value in (("ExpirationDate", datetime(2020, 1, 1)),
                           ("ProvisionedDevices", []), ("ProvisionedDevices", [3]),
                           ("TeamIdentifier", ["bad"]), ("Entitlements", {}),
                           ("ExpirationDate", "not a date")):
            with self.subTest(key=key, value=value):
                profile = {**self.profile, key: value}
                with self.assertRaises(ValueError):
                    inspect_ipa(self.archive, lambda data: profile)

    def test_missing_signature_profile_or_executable(self) -> None:
        for name in ("MinionRush", "embedded.mobileprovision", "_CodeSignature/CodeResources", "Info.plist"):
            with self.subTest(name=name):
                self.write_archive(omitted=name)
                with self.assertRaises(ValueError):
                    self.inspect()

    def test_rejects_other_runtime_and_invalid_metadata(self) -> None:
        for key, value in (("MRGuestVersion", "other"), ("CFBundleExecutable", "../bad"),
                           ("CFBundleIdentifier", "bad;identifier"), ("MinimumOSVersion", "unknown")):
            with self.subTest(key=key):
                original = self.info.copy()
                self.info[key] = value
                self.write_archive()
                with self.assertRaises(ValueError):
                    self.inspect()
                self.info = original

    def test_rejects_traversal_extra_apps_and_duplicates(self) -> None:
        for name in ("Payload/MinionRush.app/../bad", "Payload/Other.app/Info.plist",
                     "/absolute", "Payload/MinionRush.app/Info.plist", "Payload\\bad"):
            with self.subTest(name=name):
                import warnings
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", UserWarning)
                    self.write_archive({name: b"bad"})
                with self.assertRaises(ValueError):
                    self.inspect()

    def test_rejects_links_and_oversized_metadata(self) -> None:
        self.write_archive()
        with zipfile.ZipFile(self.archive, "a") as archive:
            linked = zipfile.ZipInfo("Payload/MinionRush.app/linked")
            linked.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(linked, b"elsewhere")
        with self.assertRaisesRegex(ValueError, "linked"):
            self.inspect()
        self.write_archive(omitted="Info.plist")
        with zipfile.ZipFile(self.archive, "a", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("Payload/MinionRush.app/Info.plist", b"x" * (4 * 1024**2 + 1))
        with self.assertRaisesRegex(ValueError, "Oversized"):
            self.inspect()

    def test_export_preserves_bytes_permissions_and_prior_file_on_failure(self) -> None:
        application = self.directory / "MinionRush.app"
        application.mkdir()
        binary = application / "MinionRush"
        binary.write_bytes(b"signed bytes")
        binary.chmod(0o755)
        export_ipa(application, self.archive)
        original = self.archive.read_bytes()
        with zipfile.ZipFile(self.archive) as archive:
            self.assertEqual(archive.read("Payload/MinionRush.app/MinionRush"), b"signed bytes")
            self.assertEqual(archive.getinfo("Payload/MinionRush.app/MinionRush").external_attr >> 16 & 0o777, 0o755)
        (application / "linked").symlink_to(binary)
        with self.assertRaisesRegex(ValueError, "Linked"):
            export_ipa(application, self.archive)
        self.assertEqual(self.archive.read_bytes(), original)
        self.assertFalse(list(self.directory.glob(".minion-rush-*")))


if __name__ == "__main__":
    unittest.main()
