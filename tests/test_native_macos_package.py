"""The same private ARM64 template flow used by the Windows and Mac builders."""

from __future__ import annotations

import hashlib
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from native_ipa_output import assemble_output
from native_macos_package import build_unsigned, validate_native_kit


class NativeMacPackageTests(unittest.TestCase):
    def kit(self, root: Path) -> Path:
        kit = root / "NativeKit"
        kit.mkdir()
        for name, content in (("template.zip", b"unsigned synthetic iOS template"),
                              ("a64-compiler", b"synthetic Mac executable"),
                              ("hu.xlf", b"private Hungarian synthetic catalog")):
            (kit / name).write_bytes(content)
        (kit / "a64-compiler").chmod(0o755)
        (kit / "manifest.json").write_text(json.dumps({
            "version": 1,
            "template_sha256": hashlib.sha256((kit / "template.zip").read_bytes()).hexdigest(),
            "compiler_sha256": hashlib.sha256((kit / "a64-compiler").read_bytes()).hexdigest(),
            "hungarian_sha256": hashlib.sha256((kit / "hu.xlf").read_bytes()).hexdigest(),
        }) + "\n")
        return kit

    def test_private_kit_rejects_tampering(self):
        with tempfile.TemporaryDirectory() as directory:
            kit = self.kit(Path(directory))
            self.assertEqual(validate_native_kit(kit)["version"], 1)
            (kit / "hu.xlf").write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                validate_native_kit(kit)

    def test_optional_hungarian_and_atomic_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kit = self.kit(root)
            archive = root / "original.zip"
            archive.write_bytes(b"original private synthetic archive")
            output = root / "result.ipa"
            output.write_bytes(b"older IPA")
            captured = []

            def prepare(source, destination, **kwargs):
                captured.append(kwargs)
                destination.mkdir()
            def assemble(template, private, destination):
                self.assertIn(output.read_bytes(), (b"older IPA", b"new unsigned IPA"))
                destination.write_bytes(b"new unsigned IPA")

            with (mock.patch("native_macos_package.prepare", prepare),
                  mock.patch("native_macos_package.assemble_unsigned_ipa", assemble)):
                with self.assertRaisesRegex(ValueError, "confirmation"):
                    build_unsigned(archive, output, kit, overwrite=False)
                build_unsigned(archive, output, kit, hungarian=False, overwrite=True)
                self.assertEqual(output.read_bytes(), b"new unsigned IPA")
                self.assertFalse(captured[-1]["enable_localizations"])
                self.assertIsNone(captured[-1]["translations_root"])
                build_unsigned(archive, output, kit, hungarian=True, overwrite=True)
                self.assertTrue(captured[-1]["enable_localizations"])
                self.assertEqual(captured[-1]["translations_root"], kit)

    def test_failed_candidate_preserves_previous_ipa(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "saved.ipa"
            output.write_bytes(b"precious older IPA")

            def failure(path):
                path.write_bytes(b"partial")
                raise ValueError("synthetic failure")
            with self.assertRaisesRegex(ValueError, "synthetic failure"):
                assemble_output(output, failure, overwrite=True)
            self.assertEqual(output.read_bytes(), b"precious older IPA")
            self.assertFalse(any(path.name.startswith(".mr-ipa") for path in root.iterdir()))

    def test_concurrent_destination_change_not_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "saved.ipa"
            output.write_bytes(b"original")
            def changed(path):
                path.write_bytes(b"new")
                output.write_bytes(b"external change")
            with self.assertRaisesRegex(ValueError, "changed during build"):
                assemble_output(output, changed, overwrite=True)
            self.assertEqual(output.read_bytes(), b"external change")


if __name__ == "__main__":
    unittest.main()
