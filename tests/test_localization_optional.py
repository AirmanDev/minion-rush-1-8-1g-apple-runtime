"""Runtime pack availability tests using generated, non-game test files."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HEADER = r"""
#ifndef MR_LOCALIZATION_CONFIG_H
#define MR_LOCALIZATION_CONFIG_H
#define MR_LOCALIZATION_CONFIG_COUNT 2u
#define MR_LOCALIZATION_FLAG_SLICE_COUNT 13u
#define MR_LOCALIZATION_FLAG_RECTANGLE_CAPACITY 52u
static const uint32_t TEST_FLAG[] = {0x00ff00u};
static const mr_localization_definition MR_LOCALIZATION_CONFIGS[] = {
    {"hu", "Magyar", "hu", "HU", MR_FLAG_BANDS_HORIZONTAL, TEST_FLAG, 1},
    {"xx", "Test", "xx", "XX", MR_FLAG_BANDS_HORIZONTAL, TEST_FLAG, 1},
};
#endif
"""

HARNESS = r"""
#include "localization.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc != 5) return 1;
    mr_localization_init(argv[1], argv[2], argv[3], argv[4]);
    const unsigned count = mr_localization_count();
    const int current = mr_localization_current();
    const char *code = mr_localization_code();
    printf("%u %d %s\n", count, current, code);
    for (unsigned index = 0; index < count; index++) {
        const mr_localization_definition *pack = mr_localization_get(index);
        if (!pack) return 2;
        printf("%s\n", pack->code);
    }
    return 0;
}
"""


class OptionalLocaleTests(unittest.TestCase):
    def test_runtime_only_lists_packaged_languages(self):
        if os.name == "nt":
            self.skipTest("The iOS/macOS localization runtime uses POSIX APIs")
        compiler = shutil.which("clang") or shutil.which("gcc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "localization_config.h").write_text(HEADER)
            (root / "harness.c").write_text(HARNESS)
            exe = root / ("localization-test.exe" if os.name == "nt" else "localization-test")
            subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(root), "-I", str(ROOT / "src/native"),
                 str(root / "harness.c"), str(ROOT / "src/native/localization.c"),
                 "-o", str(exe)],
                check=True, capture_output=True, text=True, timeout=30,
            )
            pack = root / "localizations"
            pack.mkdir()
            data = root / "user-data"
            data.mkdir()
            texts = root / "game/files/text"
            texts.mkdir(parents=True)

            def check(code):
                result = subprocess.run(
                    [str(exe), str(pack), str(data), "en", code],
                    check=True, capture_output=True, text=True, timeout=5,
                )
                return result.stdout.strip().splitlines()

            self.assertEqual(check("hu"), ["0 -1 en"])
            runtime = pack / "runtime/localizationText"
            runtime.mkdir(parents=True)
            (runtime / "langs.json").write_bytes(b"synthetic formatter")
            (texts / "xx.texts").write_bytes(b"synthetic xx")
            self.assertEqual(check("hu"), ["1 -1 en", "xx"])
            self.assertEqual(check("xx"), ["1 0 xx", "xx"])
            (texts / "hu.texts").write_bytes(b"synthetic hu")
            self.assertEqual(check("hu"), ["2 0 hu", "hu", "xx"])
            (texts / "xx.texts").unlink()
            self.assertEqual(check("hu"), ["1 0 hu", "hu"])


if __name__ == "__main__":
    unittest.main()
