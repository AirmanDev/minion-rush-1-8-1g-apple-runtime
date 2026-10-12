"""Portable Windows kit supply-chain and validation tests with fake inputs."""
from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
sys.dont_write_bytecode = True

import build_windows_kit
from windows_native_package import validate_kit


class WindowsKitTests(unittest.TestCase):
    def validate(self, kit: Path):
        # In production ROOT resolves to the bundled runtime/ tree.
        with mock.patch("windows_native_package.ROOT", kit / "runtime"):
            return validate_kit(kit)

    def fixture(self, root: Path):
        python_zip = root / "python-embed.zip"
        with zipfile.ZipFile(python_zip, "w") as archive:
            for name in build_windows_kit.PYTHON_FILES_REQUIRED:
                archive.writestr(name, b"synthetic test fixture")
        compiler = root / "a64-compiler.exe"
        compiler.write_bytes(b"MZ" + b"\x00" * 250)
        template = root / "unsigned-template.zip"
        template.write_bytes(b"unsigned template fixture")
        output = root / "kit"
        checksum = hashlib.sha256(python_zip.read_bytes()).hexdigest()
        with mock.patch.object(build_windows_kit, "PYTHON_3147_EMBED_AMD64_SHA256", checksum):
            build_windows_kit.build_kit(python_zip, compiler, template, output)
        return output

    def test_kit_contains_only_expected_public_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            kit = self.fixture(Path(directory))
            config = self.validate(kit)
            self.assertEqual(config["version"], 1)
            self.assertFalse((kit / "assets").exists())
            self.assertFalse((kit / "runtime/assets").exists())
            self.assertFalse((kit / "runtime/build").exists())
            self.assertTrue((kit / "python/LICENSE.txt").exists())
            self.assertIn(b"\r\n", (kit / "run-native-ipa.cmd").read_bytes())
            self.assertEqual(
                (kit / "python/python314._pth").read_bytes(),
                b"python314.zip\n.\n..\\runtime\\tools\n",
            )
            self.assertFalse((kit / "python/site-packages").exists())
            self.assertTrue((kit / "python-file-hashes.json").is_file())


    def test_rejects_changed_template_compiler_and_runtime_file(self):
        with tempfile.TemporaryDirectory() as directory:
            kit = self.fixture(Path(directory))
            for path, data in (
                (kit / "MinionRush-native-template-unsigned.zip", b"corrupted"),
                (kit / "a64-compiler.exe", b"corrupted"),
                (kit / "runtime/tools/native_ipa.py", b"corrupted"),
            ):
                original = path.read_bytes()
                path.write_bytes(data)
                try:
                    with self.assertRaisesRegex(ValueError, "checksum|integrity"):
                        self.validate(kit)
                finally:
                    path.write_bytes(original)
            self.assertEqual(self.validate(kit)["version"], 1)
            tamper = kit / "python/python314._pth"
            original = tamper.read_bytes()
            tamper.write_bytes(b"import site\n")
            with self.assertRaisesRegex(ValueError, "integrity"):
                self.validate(kit)
            tamper.write_bytes(original)
            self.assertEqual(self.validate(kit)["version"], 1)
            (kit / "runtime/tools/untrusted.py").write_text("print('bad')")
            with self.assertRaisesRegex(ValueError, "extra files"):
                self.validate(kit)


    def test_rejects_unverified_python_distribution(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "fake.zip"
            archive.write_bytes(b"untrusted")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                build_windows_kit._unpack_verified_python(archive, root / "python")


if __name__ == "__main__":
    unittest.main()
