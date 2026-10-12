"""Asset-free contracts for the optional Windows graphical IPA builder."""
from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class WindowsGuiContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gui = (ROOT / "tools/windows_native_gui.ps1").read_text(encoding="utf-8")
        cls.launcher = (ROOT / "tools/run-native-ipa.cmd").read_text(encoding="utf-8")
        cls.manifest = (ROOT / "config/source_manifest.txt").read_text(encoding="utf-8")

    def test_gui_is_part_of_verified_portable_runtime(self):
        self.assertIn("tools/windows_native_gui.ps1\n", self.manifest)
        self.assertIn("runtime\\tools\\windows_native_gui.ps1", self.launcher)
        self.assertIn("-STA", self.launcher)
        self.assertIn("-WindowStyle Hidden", self.launcher)
        self.assertIn("powershell.exe", self.launcher)

    def test_gui_delegates_entire_build_to_one_existing_backend(self):
        self.assertIn("'runtime\\tools\\windows_native_package.py'", self.gui)
        self.assertIn("'--kit'", self.gui)
        self.assertIn("'--output'", self.gui)
        self.assertIn("Quote-ProcessArgument", self.gui)
        self.assertIn("RedirectStandardOutput = $true", self.gui)
        self.assertIn("RedirectStandardError = $true", self.gui)
        self.assertNotIn("Invoke-Expression", self.gui)
        self.assertNotIn("DownloadFile", self.gui)
        self.assertNotIn("Apple ID", self.gui)

    def test_archive_location_progress_and_cancel_are_visible(self):
        for marker in (
            "Add_DragDrop", "OpenFileDialog", "SaveFileDialog",
            "'Create IPA'", "'Cancel'", "Stop-Worker",
            "ReadLineAsync()", "Remove-Scratch", "GetFullPath",
            "Directory]::Exists", "File]::Exists",
        ):
            with self.subTest(marker=marker):
                self.assertIn(marker, self.gui)

    def test_does_not_hide_unsigned_result_or_overwrite_existing_ipa(self):
        self.assertIn("unsigned IPA", self.gui)
        self.assertIn("The destination folder does not exist", self.gui)
        self.assertIn("That output already exists", self.gui)
        self.assertIn("Signing and installation happen separately", self.gui)
        self.assertIn("Not affiliated with Gameloft", self.gui)


if __name__ == "__main__":
    unittest.main()
