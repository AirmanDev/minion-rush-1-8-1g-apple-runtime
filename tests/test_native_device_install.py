"""Native IPA signing prerequisites and extraction tests."""

from __future__ import annotations

import datetime
import hashlib
import io
import plistlib
import stat
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from native_device_install import select_profile, unpack_native_app


def profile(team: str, app: str, devices: list[str], certificate: bytes) -> bytes:
    return plistlib.dumps({
        "Platform": ["iOS"],
        "TeamIdentifier": [team],
        "ExpirationDate": datetime.datetime.now(datetime.timezone.utc).replace(
            tzinfo=None) + datetime.timedelta(days=10),
        "Entitlements": {"application-identifier": f"{team}.{app}"},
        "ProvisionedDevices": devices,
        "DeveloperCertificates": [certificate],
    })


class NativeDeviceInstallTests(unittest.TestCase):
    def test_profile_matches_team_device_bundle_and_available_identity(self):
        with tempfile.TemporaryDirectory() as dirname:
            directory = Path(dirname)
            cert = b"fixture-certificate"
            identity = hashlib.sha1(cert).hexdigest().upper()
            (directory / "wildcard.mobileprovision").write_bytes(
                profile("ABCDEFGHIJ", "*", ["device1"], cert))
            (directory / "exact.mobileprovision").write_bytes(
                profile("ABCDEFGHIJ", "org.example.game", ["device1"], cert))
            result = select_profile(
                directory, "ABCDEFGHIJ", "org.example.game", "device1",
                {identity}, lambda f: f.read_bytes())
            self.assertEqual(result[0].name, "exact.mobileprovision")
            self.assertEqual(result[2], identity)
            with self.assertRaisesRegex(ValueError, "No valid signing profile"):
                select_profile(directory, "ABCDEFGHIJ", "org.example.game",
                               "other", {identity}, lambda f: f.read_bytes())

    def test_only_original_native_bundle_may_be_extracted(self):
        with tempfile.TemporaryDirectory() as dirname:
            root = Path(dirname)
            path = root / "game.ipa"
            with zipfile.ZipFile(path, "w") as z:
                for name in ("Payload/MinionRush.app/Info.plist",
                             "Payload/MinionRush.app/MinionRush"):
                    info = zipfile.ZipInfo(name)
                    info.external_attr = (stat.S_IFREG | 0o644) << 16
                    z.writestr(info, b"sample")
            app = unpack_native_app(path, root / "work")
            self.assertTrue((app / "MinionRush").is_file())
            self.assertTrue((app / "MinionRush").stat().st_mode & 0o111)

    def test_rejects_zip_traversal(self):
        with tempfile.TemporaryDirectory() as dirname:
            root = Path(dirname)
            path = root / "bad.ipa"
            with zipfile.ZipFile(path, "w") as z:
                info = zipfile.ZipInfo("Payload/MinionRush.app/../../unsafe")
                info.external_attr = (stat.S_IFREG | 0o644) << 16
                z.writestr(info, b"unsafe")
            with self.assertRaisesRegex(ValueError, "Invalid generated IPA"):
                unpack_native_app(path, root / "work")


if __name__ == "__main__":
    unittest.main()
