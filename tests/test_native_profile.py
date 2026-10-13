"""Game-independent host profile contract tests for the AOT iOS runtime."""
from __future__ import annotations

import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

from native_profile import (HEADER, MAX_CODE_BYTES, create_profile, inspect_profile,
                            symbol_names)
from build_generic_bindings import render
from test_native_ipa import synthetic_compiled

SYMBOLS = TOOLS.parent / "src/native/game_symbols.def"


class NativeProfileTests(unittest.TestCase):
    def test_local_compiler_output_to_binary_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            compiled = synthetic_compiled(Path(directory))
            profile = create_profile(compiled, SYMBOLS)
            result = inspect_profile(profile, SYMBOLS)
            self.assertEqual(result["symbols"], len(symbol_names(SYMBOLS)))
            self.assertEqual(result["blocks"], 1)
            self.assertEqual(result["code_size"], 256)
            self.assertEqual(result["blocked"], 2)
            self.assertEqual(result["hooks"], 3)
            self.assertEqual(profile[HEADER.size + 4 * result["symbols"] + 8], 0)

    def test_profile_tampering_and_truncation_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = create_profile(synthetic_compiled(Path(directory)), SYMBOLS)
            for candidate in (profile[:-1], profile + b"x", b"", b"bad" + profile[3:]):
                with self.subTest(size=len(candidate)):
                    with self.assertRaises(ValueError):
                        inspect_profile(candidate, SYMBOLS)
            modified = bytearray(profile)
            modified[-1] ^= 1
            with self.assertRaisesRegex(ValueError, "checksum"):
                inspect_profile(modified, SYMBOLS)

    def test_rejects_private_aot_map_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            compiled = synthetic_compiled(Path(directory))
            m = compiled / "minion_map.inc"
            m.write_text(m.read_text().replace("1111111111111111", "2222222222222222"))
            with self.assertRaisesRegex(ValueError, "fingerprints"):
                create_profile(compiled, SYMBOLS)

    def test_rejects_out_of_bounds_aot_and_invalid_block(self):
        with tempfile.TemporaryDirectory() as directory:
            compiled = synthetic_compiled(Path(directory))
            m = compiled / "minion_map.inc"
            m.write_text(m.read_text().replace("0u)", "256u)"))
            with self.assertRaisesRegex(ValueError, "offset"):
                create_profile(compiled, SYMBOLS)
            (compiled / "minion_code.bin").write_bytes(b"abc")
            with self.assertRaisesRegex(ValueError, "slot"):
                create_profile(compiled, SYMBOLS)

    def test_rejects_missing_binding_and_duplicate_map_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            compiled = synthetic_compiled(Path(directory))
            file = compiled / "game_bindings.h"
            lines = file.read_text().splitlines()
            file.write_text("\n".join(line for line in lines
                                      if not line.startswith("#define MR_GAME_JNI_ON_LOAD "))+"\n")
            with self.assertRaisesRegex(ValueError, "binding definitions"):
                create_profile(compiled, SYMBOLS)
            synthetic_compiled_alt = Path(directory) / "fresh"
            synthetic_compiled_alt.mkdir()
            compiled = synthetic_compiled(synthetic_compiled_alt)
            m = compiled / "minion_map.inc"
            m.write_text(m.read_text()+"A32_BLOCK(0x00001000u, 4u)\n")
            with self.assertRaisesRegex(ValueError, "block address"):
                create_profile(compiled, SYMBOLS)

    @unittest.skipUnless(sys.platform == "darwin" and shutil.which("clang"),
                         "Apple CommonCrypto native parser test requires macOS Clang")
    def test_native_ios_profile_loader_against_host_generated_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compiled = synthetic_compiled(root)
            profile = create_profile(compiled, SYMBOLS)
            good = root / "good.profile"
            bad = root / "bad.profile"
            good.write_bytes(profile)
            corrupted = bytearray(profile)
            corrupted[-1] ^= 1
            bad.write_bytes(corrupted)
            (root / "game_profile_limits.h").write_text(
                "#define MR_AOT_MAX_BLOCKS 1200000u\n"
                "#define MR_AOT_MAX_BLOCKED 1024u\n"
                "#define MR_AOT_MAX_HOOKS 2048u\n"
                "#define MR_AOT_CODE_SLOT_BYTES 201326592u\n", encoding="ascii")
            (root / "loader_test.c").write_text(
                "#include \"game_profile.h\"\n"
                "int main(int argc, char **argv) {\n"
                "  if (argc != 2) return 2;\n"
                "  if (mr_game_profile_load(argv[1]) != 0) return 1;\n"
                "  return mr_profile_block_count == 1 && "
                "mr_profile_bindings[MR_NATIVE_PROFILE_JNI_ON_LOAD] == 1 ? 0 : 3;\n"
                "}\n", encoding="ascii")
            executable = root / "loader_test"
            subprocess.run(
                ["clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-I", str(root), "-I", str(TOOLS.parent / "src/native"),
                 str(TOOLS.parent / "src/native/game_profile.c"),
                 str(root / "loader_test.c"), "-o", str(executable)],
                check=True, capture_output=True, timeout=45)
            self.assertEqual(subprocess.run(
                [str(executable), str(good)], capture_output=True, timeout=10).returncode, 0)
            self.assertEqual(subprocess.run(
                [str(executable), str(bad)], capture_output=True, timeout=10).returncode, 1)

    def test_public_header_has_no_engine_specific_addresses(self):
        source = render(SYMBOLS)
        self.assertIn("#define MR_GAME_JNI_ON_LOAD", source)
        self.assertEqual(source.count("(mr_profile_bindings["), len(symbol_names(SYMBOLS)))
        self.assertNotIn("#define MR_GAME_JNI_ON_LOAD 0x", source)
        self.assertNotIn("MR_GAME_BLOCK_HASH UINT64_C", source)


if __name__ == "__main__":
    unittest.main()
