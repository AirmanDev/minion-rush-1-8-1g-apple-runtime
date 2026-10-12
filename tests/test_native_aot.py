"""Cross-platform AOT prep tests using synthetic, unlicensed files only."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
sys.dont_write_bytecode = True

from prepare_native_aot import (AOT_PRODUCTS, check_destination,
                                compile_host_compiler, prepare)


class NativeAotTests(unittest.TestCase):
    def test_host_compiler_builds_and_rejects_invalid_elf(self) -> None:
        if not shutil.which("clang"):
            self.skipTest("Host Clang not installed")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compiler = root / ("a64-compiler.exe" if os.name == "nt" else "a64-compiler")
            compile_host_compiler(compiler)
            self.assertGreater(compiler.stat().st_size, 20_000)
            source = root / "invalid.so"
            source.write_bytes(b"invalid test data")
            output_paths = [str(root / name) for name in AOT_PRODUCTS]
            result = subprocess.run(
                [str(compiler), str(source), *output_paths],
                text=True, capture_output=True, timeout=20,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(any(Path(path).exists() for path in output_paths))

    def test_destination_refuses_existing_or_public_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory).resolve()
            self.assertEqual(check_destination(parent / "private"), parent / "private")
            existing = parent / "existing"
            existing.mkdir()
            with self.assertRaisesRegex(ValueError, "new"):
                check_destination(existing)
            with self.assertRaisesRegex(ValueError, "absolute"):
                check_destination(Path("relative"))
            with self.assertRaisesRegex(ValueError, "public|private"):
                check_destination(TOOLS.parent / "src" / "not-for-git")
            linked = parent / "linked"
            try:
                linked.symlink_to(existing, target_is_directory=True)
            except OSError:
                pass  # Symlink privileges are not guaranteed on Windows.
            else:
                with self.assertRaisesRegex(ValueError, "parent"):
                    check_destination(linked / "private")

    def test_private_outputs_are_atomic_and_not_republished(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            archive = root / "original.zip"
            archive.write_bytes(b"test archive fixture")
            target = root / "private"
            def stub_compile(compiler: Path, cc: str = "clang") -> None:
                compiler.write_bytes(b"compiler placeholder")
            def stub_import(source: Path, project: Path, assets: Path) -> None:
                engine = assets / "lib/libdespicablemefree.so"
                engine.parent.mkdir(parents=True)
                engine.write_bytes(b"fake engine fixture")

            def stub_run(args, **kwargs):
                for name in AOT_PRODUCTS:
                    Path(args[AOT_PRODUCTS.index(name) + 2]).write_bytes(b"private fixture")
                return subprocess.CompletedProcess(args, 0)

            with (mock.patch("prepare_native_aot.compile_host_compiler", stub_compile),
                  mock.patch("prepare_native_aot.install_release", stub_import),
                  mock.patch("prepare_native_aot.subprocess.run", stub_run)):
                details = prepare(archive, target)
            self.assertTrue(details["not_an_ipa"])
            self.assertEqual(set(details["products_sha256"]), set(AOT_PRODUCTS))
            self.assertTrue((target / "assets/lib/libdespicablemefree.so").is_file())
            self.assertFalse((target / "a64-compiler").exists())
            self.assertFalse((target / "a64-compiler.exe").exists())
            self.assertEqual(
                sorted(path.name for path in (target / "compiled").iterdir()),
                sorted(AOT_PRODUCTS),
            )
            with self.assertRaisesRegex(ValueError, "new"):
                prepare(archive, target)

    def test_rejects_untrusted_prebuilt_windows_compiler(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            archive = root / "original.zip"
            archive.write_bytes(b"synthetic archive")
            compiler = root / "a64-compiler.exe"
            compiler.write_bytes(b"untrusted executable")
            output = root / "private"
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                prepare(archive, output, prebuilt_compiler=compiler,
                        compiler_sha256="0" * 64)
            self.assertFalse(output.exists())

    def test_failed_conversion_does_not_leave_partial_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            archive = root / "original.zip"
            archive.write_bytes(b"synthetic")
            target = root / "private"
            with (mock.patch("prepare_native_aot.compile_host_compiler",
                             side_effect=ValueError("no compiler")),):
                with self.assertRaisesRegex(ValueError, "no compiler"):
                    prepare(archive, target)
            self.assertFalse(target.exists())
            self.assertFalse(any(path.name.startswith(".mr-private-") for path in root.iterdir()))


if __name__ == "__main__":
    unittest.main()
