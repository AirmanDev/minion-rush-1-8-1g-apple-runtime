"""Portable Mac Python pinning and self-contained executable selection."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from third_party_notices import COMMON_NOTICES, MAC_ONLY_NOTICES, TOOLCHAIN_NOTICES, verify_notice
from mac_portable_runtime import (ARCHIVE_SHA256, PYTHON_VERSION, install_runtime,
                                  SKIP_ROOTS, SKIP_STDLIB)


class PortableMacRuntimeTests(unittest.TestCase):
    def test_python_version_and_upstream_sha256_pinned(self):
        self.assertEqual(PYTHON_VERSION, "3.14.8")
        self.assertEqual(
            ARCHIVE_SHA256,
            "ed14e9042f36c0aa18383f5d9729bcbf17847e842666a793b5a5a5d8505c0ca1",
        )

    def test_dependency_notices_are_pinned(self):
        for name, digest in COMMON_NOTICES.items():
            verify_notice(ROOT / "third_party_licenses" / name, digest)
        for name, digest in TOOLCHAIN_NOTICES.items():
            verify_notice(ROOT / "third_party_licenses" / name, digest)
        for name, digest in MAC_ONLY_NOTICES.items():
            verify_notice(ROOT / "third_party_licenses/mac_python" / name, digest)

    def test_modified_or_missing_archive_is_refused_without_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "modified.tar.gz"
            archive.write_bytes(b"untrusted")
            destination = root / "PythonRuntime"
            with self.assertRaisesRegex(ValueError, "checksum"):
                install_runtime(archive, destination)
            self.assertFalse(destination.exists())

    def test_nonruntime_python_modules_are_excluded(self):
        self.assertIn("tkinter", SKIP_STDLIB)
        self.assertIn("site-packages", SKIP_STDLIB)
        self.assertIn("ensurepip", SKIP_STDLIB)
        self.assertIn("tk9.0", SKIP_ROOTS)
        self.assertNotIn("encodings", SKIP_STDLIB)

    def test_private_mac_app_must_select_own_interpreter(self):
        backend = (ROOT / "installer/BackendProcess.swift").read_text()
        self.assertIn('NativeKit/manifest.json', backend)
        self.assertIn('PythonRuntime/bin/python3.14', backend)
        self.assertIn('throw CocoaError(.executableNotLoadable)', backend)
        self.assertIn('return bundled', backend)


if __name__ == "__main__":
    unittest.main()
