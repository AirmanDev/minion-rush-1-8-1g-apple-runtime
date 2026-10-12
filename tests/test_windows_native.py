"""Portable Windows kit supply-chain and validation tests with fake inputs."""
from __future__ import annotations

import hashlib
import json
import os
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
from windows_native_package import (_needs_local_archive_copy,
                                    _stage_source_archive, create_unsigned_ipa,
                                    validate_kit)


class WindowsKitTests(unittest.TestCase):
    def validate(self, kit: Path):
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


    def test_remote_archive_is_staged_for_seekable_local_zip_reads(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "original.zip"
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("synthetic-test.txt", b"private test bytes")
            target = root / "working"
            target.mkdir()
            with mock.patch("windows_native_package._needs_local_archive_copy",
                            return_value=True):
                copied = _stage_source_archive(archive, target)
            self.assertEqual(copied, target / "original-private-source.zip")
            self.assertEqual(copied.read_bytes(), archive.read_bytes())
            with zipfile.ZipFile(copied) as opened:
                self.assertEqual(opened.read("synthetic-test.txt"), b"private test bytes")
            self.assertEqual(len(list(target.iterdir())), 1)

    def test_local_zip_skips_unnecessary_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "original.zip"
            archive.write_bytes(b"not a real original archive")
            self.assertIs(_stage_source_archive(archive, root), archive)
            self.assertEqual(len(list(root.iterdir())), 1)

    def test_staged_zip_has_strict_bound_and_stays_outside_kit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "original.zip"
            source.write_bytes(b"123456789")
            scratch = root / "scratch"
            scratch.mkdir()
            with (mock.patch("windows_native_package._needs_local_archive_copy",
                             return_value=True),
                  mock.patch("windows_native_package.MAX_ARCHIVE_BYTES", 8)):
                with self.assertRaisesRegex(ValueError, "exceeds"):
                    _stage_source_archive(source, scratch)
            self.assertEqual(source.read_bytes(), b"123456789")
            self.assertFalse((root / "kit").exists())

    def test_full_builder_uses_staged_zip_and_cleans_up(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            private_zip = root / "original.zip"
            private_zip.write_bytes(b"synthetic private source zip")
            kit = root / "kit"
            kit.mkdir()
            output = root / "final.ipa"
            def verify_prepare(path, output_path, **kwargs):
                self.assertEqual(path.name, "original-private-source.zip")
                self.assertEqual(path.read_bytes(), private_zip.read_bytes())
                self.assertNotEqual(path.parent, kit)
            def verify_assemble(template, private, dest):
                dest.write_bytes(b"synthetic unsigned IPA")
            with (mock.patch("windows_native_package.validate_kit",
                             return_value={"compiler_sha256": "0" * 64}),
                  mock.patch("windows_native_package._needs_local_archive_copy",
                             return_value=True),
                  mock.patch("windows_native_package.prepare", verify_prepare),
                  mock.patch("windows_native_package.assemble_unsigned_ipa", verify_assemble)):
                create_unsigned_ipa(private_zip, kit, output)
            self.assertEqual(output.read_bytes(), b"synthetic unsigned IPA")
            self.assertEqual(private_zip.read_bytes(), b"synthetic private source zip")
            self.assertFalse(list(root.glob("minion-rush-private-*")))

    @unittest.skipUnless(os.name == "nt", "Windows-specific UNC detection")
    def test_unc_path_automatically_requests_local_copy(self):
        from pathlib import PureWindowsPath
        self.assertTrue(_needs_local_archive_copy(
            PureWindowsPath(r"\\localhost@9843\DavWWWRoot\original-1.8.1g-PRIVATE.zip")
        ))

    def test_rejects_unverified_python_distribution(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "fake.zip"
            archive.write_bytes(b"untrusted")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                build_windows_kit._unpack_verified_python(archive, root / "python")


if __name__ == "__main__":
    unittest.main()
