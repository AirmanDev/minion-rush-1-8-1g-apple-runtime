"""Native Mac and Windows use one maintained color contract."""

from __future__ import annotations

import json
import plistlib
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

    def test_portable_builder_title_describes_independent_tool(self):
        config = json.loads((ROOT / "config/installer_ui.json").read_text())
        info = plistlib.loads((ROOT / "installer/Info.plist").read_bytes())
        windows = (ROOT / "src/windows/ipa_builder.c").read_text()
        mac = (ROOT / "installer/InstallerApp.swift").read_text()
        self.assertEqual(config["title"], "Unofficial IPA Builder")
        self.assertEqual(info["CFBundleDisplayName"], config["title"])
        self.assertIn('Window("Unofficial IPA Builder"', mac)
        self.assertIn('MR_BUILDER_TITLE L"Unofficial IPA Builder"', windows)
        self.assertIn("Minion Rush 1.8.1g", config["labels"]["headerSubtitle"])
        self.assertIn("For Minion Rush 1.8.1g", windows)
        self.assertEqual(windows.count('{L"For Minion Rush 1.8.1g. Use your own original ZIP."'), 1)

    def test_desktop_versions_have_one_source(self):
        info = plistlib.loads((ROOT / "installer/Info.plist").read_bytes())
        windows_build = (ROOT / "tools/build_windows_gui.sh").read_text()
        windows_ui = (ROOT / "src/windows/ipa_builder.c").read_text()
        mac_ui = (ROOT / "installer/InstallerView.swift").read_text()
        self.assertRegex(info["CFBundleShortVersionString"], r"^\d+\.\d+\.\d+$")
        self.assertIn("CFBundleShortVersionString", windows_build)
        self.assertIn("MR_BUILDER_VERSION", windows_build)
        self.assertIn('L"v" MR_BUILDER_VERSION', windows_ui)
        self.assertIn('Text("v" + model.appVersion)', mac_ui)

    def test_mac_activity_log_only_scrolls_vertically(self):
        view = (ROOT / "installer/InstallerView.swift").read_text()
        self.assertIn("ScrollView(.vertical)", view)
        self.assertNotIn("ScrollView([.horizontal, .vertical])", view)
        self.assertIn(".fixedSize(horizontal: false, vertical: true)", view)
        self.assertIn('Button(text("installGame"))', view)
        self.assertNotIn("signedSection", view)

    def test_tokens_cover_all_native_mac_colors(self):
        theme = json.loads((ROOT / "config/builder_theme.json").read_text())
        self.assertEqual(set(theme), {name for name, _ in NAMES})
        for expected in ("navy", "background", "card", "border",
                         "gold", "success", "failure"):
            self.assertIn(expected, theme)


if __name__ == "__main__":
    unittest.main()
