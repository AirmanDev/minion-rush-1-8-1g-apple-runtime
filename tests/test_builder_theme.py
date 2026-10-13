"""Native Mac and Windows use one maintained color contract."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from build_builder_palette import build_files, NAMES


class BuilderThemeTests(unittest.TestCase):
    def test_generated_native_palettes_match_single_source(self):
        files = build_files()
        self.assertEqual(len(files), 2)
        for path, expected in files.items():
            with self.subTest(path=path):
                self.assertEqual(path.read_text(encoding="ascii"), expected)

    def test_tokens_cover_all_native_mac_colors(self):
        theme = json.loads((ROOT / "config/builder_theme.json").read_text())
        self.assertEqual(set(theme), {name for name, _ in NAMES})
        for expected in ("navy", "background", "card", "border",
                         "gold", "success", "failure"):
            self.assertIn(expected, theme)


if __name__ == "__main__":
    unittest.main()
