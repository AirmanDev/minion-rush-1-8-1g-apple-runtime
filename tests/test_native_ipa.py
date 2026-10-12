"""Synthetic Mach-O / unsigned IPA tests; no proprietary input required."""
from __future__ import annotations

import hashlib
import json
import plistlib
import stat
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

from native_macho import find_code_slot, require_zero_slot, normalized_text_sha256
from native_ipa import (
    APP_NAME, TEMPLATE_MANIFEST, TEMPLATE_PREFIX, _check_template_zip,
    assemble_unsigned_ipa, create_template, make_neutral_icon,
)


def macho(slot_size=256):
    # Minimal thin ARM64 executable with __MRCODE/__text mapped as RX regular.
    header = struct.pack("<IiiIIIII", 0xFEEDFACF, 0x0100000C, 0, 2, 1, 152, 0, 0)
    segment = struct.pack(
        "<II16sQQQQiiII", 0x19, 152, b"__MRCODE", 0x100000000,
        0x1000, 0x1000, slot_size, 5, 5, 1, 0,
    )
    section = struct.pack(
        "<16s16sQQIIIIIIII", b"__text", b"__MRCODE", 0x100000000,
        slot_size, 0x1000, 12, 0, 0, 0x80000400, 0, 0, 0,
    )
    return header + segment + section + bytes(0x1000 - len(header) - len(segment) - len(section)) + bytes(slot_size)


def synthetic_app(root: Path) -> Path:
    app = root / APP_NAME
    app.mkdir()
    (app / "MinionRush").write_bytes(macho())
    (app / "Info.plist").write_bytes(plistlib.dumps({
        "CFBundleExecutable": "MinionRush",
        "CFBundleIdentifier": "example.native.runtime",
    }))
    (app / "PrivacyInfo.xcprivacy").write_text("{}")
    return app


def synthetic_compiled(root: Path) -> Path:
    compiled = root / "compiled"
    compiled.mkdir()
    for name, value in (
        ("minion_code.bin", b"\x1f\x20\x03\xd5" * 64),
        ("minion_map.inc", b"A32_BLOCK(0x1000u, 0u)\n"),
        ("game_bindings.h", b"#define MR_GAME_BINDING_HASH 0u\n"),
    ):
        (compiled / name).write_bytes(value)
    return compiled


class NativeMachoTests(unittest.TestCase):
    def test_find_and_check_zero_slot(self):
        data = macho()
        slot = find_code_slot(data)
        self.assertEqual((slot.offset, slot.size), (0x1000, 256))
        require_zero_slot(data, slot)
        data = data[:0x1000] + b"\x01" + data[0x1001:]
        with self.assertRaisesRegex(ValueError, "nonzero"):
            require_zero_slot(data, slot)

    def test_rejects_truncation_invalid_cpu_nonexecutable_and_bad_ranges(self):
        samples = [
            macho()[:50],
            bytearray(macho()),
            bytearray(macho()),
            bytearray(macho()),
            bytearray(macho()),
        ]
        struct.pack_into("<I", samples[1], 4, 0x01000007)
        struct.pack_into("<I", samples[2], 32 + 60, 1)  # RX protection removed
        struct.pack_into("<I", samples[3], 32 + 64, 257)
        struct.pack_into("<Q", samples[4], 32 + 40, 0xFFFF_FFFF)  # enormous file offset
        for data in samples:
            with self.subTest(index=str(data[:44])):
                with self.assertRaises(ValueError):
                    find_code_slot(bytes(data))

    def test_text_newline_normalization(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "map.inc"
            path.write_bytes(b"a\r\nb\r\nc\r\n")
            self.assertEqual(normalized_text_sha256(path),
                             hashlib.sha256(b"a\nb\nc\n").hexdigest())


class NativeIpaTests(unittest.TestCase):
    def fixture(self, root: Path):
        app = synthetic_app(root)
        compiled = synthetic_compiled(root)
        template = root / "unsigned-template.zip"
        create_template(app, compiled, template)
        return app, compiled, template

    def test_template_is_asset_free_and_has_verified_empty_slot(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            _, _, template = self.fixture(root)
            with zipfile.ZipFile(template) as archive:
                files = _check_template_zip(archive)
                self.assertEqual(len(files), 4)
                self.assertNotIn("lib", "\n".join(files))
                manifest = json.loads(archive.read(TEMPLATE_MANIFEST))
                self.assertEqual(manifest["slot_size"], 256)
                self.assertEqual(manifest["slot_offset"], 0x1000)

    def test_template_rejects_original_assets_and_nonempty_code(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            app = synthetic_app(root)
            compiled = synthetic_compiled(root)
            (app / "game").mkdir()
            (app / "game" / "private.bin").write_bytes(b"secret")
            with self.assertRaisesRegex(ValueError, "game files"):
                create_template(app, compiled, root / "template.zip")
            (app / "game" / "private.bin").unlink()
            (app / "game").rmdir()
            executable = app / "MinionRush"
            data = bytearray(executable.read_bytes())
            data[0x1000] = 1
            executable.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "nonzero"):
                create_template(app, compiled, root / "template.zip")

    def test_template_rejects_archive_path_traversal_and_duplicates(self):
        with tempfile.TemporaryDirectory() as directory:
            template = Path(directory) / "template.zip"
            for names in (
                [TEMPLATE_MANIFEST, TEMPLATE_PREFIX+"../malicious"],
                [TEMPLATE_MANIFEST, TEMPLATE_MANIFEST],
                [TEMPLATE_MANIFEST, TEMPLATE_PREFIX+"lib/game.so"],
            ):
                with zipfile.ZipFile(template, "w") as archive:
                    for name in names:
                        archive.writestr(name, b"")
                with self.assertRaises(ValueError):
                    with zipfile.ZipFile(template) as archive:
                        _check_template_zip(archive)

    def test_assemble_is_per_user_unsigned_and_aligned(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            _, compiled, template = self.fixture(root)
            private = root / "private"
            private.mkdir()
            (private / "compiled").mkdir()
            for file in compiled.iterdir():
                (private / "compiled" / file.name).write_bytes(file.read_bytes())
            assets = private / "assets"
            assets.mkdir()
            (assets / "lib").mkdir()
            (assets / "lib/game.so").write_bytes(b"synthetic private ELF")
            output = root / "private.ipa"
            with mock.patch("native_ipa._validate_private_assets",
                            return_value=[(assets / "lib/game.so", Path("lib/game.so"))]):
                assemble_unsigned_ipa(template, private, output)
            with zipfile.ZipFile(output) as archive:
                self.assertIn("Payload/MinionRush.app/lib/game.so", archive.namelist())
                binary = archive.read("Payload/MinionRush.app/MinionRush")
                self.assertEqual(binary[0x1000:0x1100], (compiled / "minion_code.bin").read_bytes())
                self.assertFalse(any("_CodeSignature" in item for item in archive.namelist()))

    def test_cannot_overwrite_existing_ipa_or_mismatched_compiled_code(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            _, compiled, template = self.fixture(root)
            private = root / "private"
            private.mkdir()
            (private / "compiled").mkdir()
            for path in compiled.iterdir():
                (private / "compiled" / path.name).write_bytes(path.read_bytes())
            (private / "assets").mkdir()
            output = root / "output.ipa"
            output.write_bytes(b"keep")
            with self.assertRaisesRegex(ValueError, "new .ipa"):
                assemble_unsigned_ipa(template, private, output)
            self.assertEqual(output.read_bytes(), b"keep")
            output.unlink()
            (private / "compiled/minion_code.bin").write_bytes(b"\x01" * 256)
            with mock.patch("native_ipa._validate_private_assets", return_value=[]):
                with self.assertRaisesRegex(ValueError, "match"):
                    assemble_unsigned_ipa(template, private, output)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
