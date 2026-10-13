"""Windows Builder icon and end-of-build status source contracts."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from verify_windows_icon import verify_icon


class WindowsIconTests(unittest.TestCase):
    def test_app_icon_has_required_sizes(self):
        self.assertEqual(verify_icon(ROOT / "src/windows/builder.ico"), 7)
        self.assertIn('101 ICON "src/windows/builder.ico"',
                      (ROOT / "src/windows/builder.rc").read_text())

    def test_windows_icon_uses_single_mac_artwork_generator(self):
        builder = (ROOT / "tools/build_windows_icon.py").read_text()
        self.assertIn("prepare_installer_icon.swift", builder)
        self.assertIn("render_mac_iconset(working)", builder)
        self.assertNotIn("def render_pixel(", builder)
        self.assertNotIn("ARROW =", builder)

    def test_finished_diagnostics_update_before_success_message(self):
        source = (ROOT / "src/windows/ipa_builder.c").read_text()
        begin = source.index("static void finish_build(DWORD exit_code)")
        end = source.index("static void pick_into(", begin)
        finished = source[begin:end]
        self.assertLess(finished.index("refresh_diagnostics();"),
                        finished.index("MessageBoxW(main_window"))
        self.assertIn("set_status(STATE_OK", finished)

    def test_gui_and_launcher_link_same_resource(self):
        source = (ROOT / "tools/build_windows_gui.sh").read_text()
        self.assertIn("x86_64-w64-mingw32-windres", source)
        self.assertEqual(source.count('"$OUT/windows/builder_icon.o"'), 3)


if __name__ == "__main__":
    unittest.main()
