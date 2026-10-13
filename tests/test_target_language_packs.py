"""Target-only language pack validation, using synthetic source strings."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from localize import TextEntry, encode_texts, read_target_pack


class TargetPackTests(unittest.TestCase):
    def setUp(self):
        self.source = [TextEntry("A", "Hello %s"), TextEntry("B", "Play")]
        self.reference = hashlib.sha256(encode_texts(self.source)).hexdigest()

    def write(self, directory: Path, code: str, targets: list[str],
              reference: str | None = None) -> Path:
        path = directory / (code + ".json")
        path.write_text(json.dumps({
            "version": 1, "language": code, "source_sha256": reference or self.reference,
            "targets": targets,
        }, ensure_ascii=True))
        return path

    def test_multiple_additive_language_packs_preserve_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            hu = self.write(root, "hu", ["Szia %s", "Jatek"])
            xx = self.write(root, "xx", ["Bonjour %s", "Jouer"])
            original = encode_texts(self.source)
            self.assertEqual([entry.value for entry in read_target_pack(hu, self.source, "hu")],
                             ["Szia %s", "Jatek"])
            self.assertEqual(len(read_target_pack(xx, self.source, "xx")), 2)
            self.assertEqual(encode_texts(self.source), original)
            self.assertNotIn("Hello %s", hu.read_text())

    def test_rejects_wrong_game_or_missing_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write(Path(directory), "hu", ["Szia %s", "Jatek"], "0" * 64)
            with self.assertRaisesRegex(ValueError, "does not match"):
                read_target_pack(path, self.source, "hu")
            self.write(Path(directory), "hu", ["Szia %s"])
            with self.assertRaisesRegex(ValueError, "wrong entry count"):
                read_target_pack(path, self.source, "hu")

    def test_rejects_placeholder_mismatch_and_unexpected_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write(Path(directory), "hu", ["Szia", "Jatek"])
            with self.assertRaisesRegex(ValueError, "placeholder mismatch"):
                read_target_pack(path, self.source, "hu")
            data = json.loads(path.read_text())
            data["source"] = ["Hello %s", "Play"]
            path.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "unknown additional"):
                read_target_pack(path, self.source, "hu")


if __name__ == "__main__":
    unittest.main()
