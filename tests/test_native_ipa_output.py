"""Cross-platform destination safety for unsigned IPA generation."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from native_ipa_output import assemble_output, validate_output_destination


class SharedIpaOutputTests(unittest.TestCase):
    def test_rejects_bad_extension_and_nonabsolute_path(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "absolute"):
                validate_output_destination(Path("output.ipa"))
            with self.assertRaisesRegex(ValueError, "absolute"):
                validate_output_destination(Path(directory) / "output.zip")

    def test_new_output_is_atomic_and_does_not_leave_partial_file(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "new.ipa"
            def incomplete(candidate):
                self.assertFalse(output.exists())
                candidate.write_bytes(b"partial")
                raise RuntimeError("staged failure")
            with self.assertRaisesRegex(RuntimeError, "staged failure"):
                assemble_output(output, incomplete)
            self.assertFalse(output.exists())
            assemble_output(output, lambda candidate: candidate.write_bytes(b"complete"))
            self.assertEqual(output.read_bytes(), b"complete")
            self.assertFalse(any(item.name.startswith(".mr-ipa-") for item
                                 in Path(directory).iterdir()))

    def test_existing_output_requires_confirmation_and_preserves_on_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "old.ipa"
            output.write_bytes(b"original")
            with self.assertRaisesRegex(ValueError, "confirmation"):
                validate_output_destination(output)
            def broken(candidate):
                candidate.write_bytes(b"new")
                raise RuntimeError("synthetic failure")
            with self.assertRaises(RuntimeError):
                assemble_output(output, broken, overwrite=True)
            self.assertEqual(output.read_bytes(), b"original")

    def test_detects_destination_changed_during_assembly(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "old.ipa"
            output.write_bytes(b"old")
            def external_update(candidate):
                candidate.write_bytes(b"generated")
                output.write_bytes(b"someone else changed it")
            with self.assertRaisesRegex(ValueError, "changed"):
                assemble_output(output, external_update, overwrite=True)
            self.assertEqual(output.read_bytes(), b"someone else changed it")

    def test_no_overwrite_existing_and_reject_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "folder.ipa"
            output.mkdir()
            with self.assertRaisesRegex(ValueError, "confirmation"):
                validate_output_destination(output, overwrite=True)
            target = root / "original.ipa"
            target.write_bytes(b"original")
            link = root / "linked.ipa"
            try:
                link.symlink_to(target)
            except (OSError, NotImplementedError):
                self.skipTest("symlinks not available")
            with self.assertRaisesRegex(ValueError, "linked"):
                validate_output_destination(link, overwrite=True)
            self.assertEqual(target.read_bytes(), b"original")


if __name__ == "__main__":
    unittest.main()
