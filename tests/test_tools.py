from __future__ import annotations

import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
sys.dont_write_bytecode = True

from common import content_files, is_disposable, load_json  # noqa: E402
from configure_graphics import apply_configuration, validate  # noqa: E402
from install_assets import extract_release_archive  # noqa: E402
from list_ios_devices import paired_physical_devices  # noqa: E402
from localize import (  # noqa: E402
    Catalog,
    TextEntry,
    create_xliff,
    encode_texts,
    generated_header,
    read_texts,
    read_xliff,
    token_signature,
)
from run_with_timeout import run as run_with_timeout  # noqa: E402
from select_ios_simulators import select  # noqa: E402
from validate_assets import validate_jpk_archives  # noqa: E402
from validate_source import IGNORED_ROOTS  # noqa: E402
from verify_ios_startup import startup_state, valid_png  # noqa: E402


class CommonTests(unittest.TestCase):
    def test_disposable_names(self) -> None:
        for name in (".DS_Store", "._icon", "cache-wal", "cache-shm", "x.updated"):
            self.assertTrue(is_disposable(name))
        self.assertFalse(is_disposable("profiles.json"))

    def test_content_files_are_sorted_and_filtered(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "z.txt").write_text("z", encoding="utf-8")
            (root / ".DS_Store").write_text("metadata", encoding="utf-8")
            (root / "a.txt").write_text("a", encoding="utf-8")
            names = [path.name for path in content_files(root)]
        self.assertEqual(names, ["a.txt", "z.txt"])

    def test_source_scan_ignores_xcode_user_data(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            user_data = root / "App.xcodeproj" / "xcuserdata" / "user.xcuserdatad"
            user_data.mkdir(parents=True)
            (user_data / "UserInterfaceState.xcuserstate").write_bytes(b"state")
            (root / "source.c").write_text("int value;", encoding="utf-8")
            paths = [
                path.relative_to(root).as_posix()
                for path in content_files(root, IGNORED_ROOTS)
            ]
        self.assertEqual(paths, ["source.c"])

    def test_json_falls_back_to_latin1(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "latin1.json"
            path.write_bytes(b'{"name":"caf\xe9"}')
            self.assertEqual(load_json(path), {"name": "caf\u00e9"})

    def test_applies_one_graphics_configuration_to_every_profile(self) -> None:
        config = {
            "host": {
                "render_scale": 1.0,
                "msaa_samples": 2,
                "anisotropy": 4,
                "pbo": True,
                "engine_hz": 60,
            },
            "game": {
                "cpu": {"feature": 1},
                "gpu": {"quality": 1},
                "texture_budget_mb": 128,
            },
        }
        source = {
            "Profiles": {
                "CPU_0": {"feature": 0},
                "CPU_2": {"feature": 1},
                "GPU_0": {"quality": 0},
                "GPU_5": {"quality": 1},
                "MEM_200": {"textureBudget": 50},
            }
        }
        configured = apply_configuration(config, source)
        validate(config, configured)
        self.assertEqual(configured["Profiles"]["CPU_0"], {"feature": 1})
        self.assertEqual(configured["Profiles"]["GPU_0"], {"quality": 1})
        self.assertEqual(
            configured["Profiles"]["MEM_200"], {"textureBudget": 128}
        )

    def test_foreground_timeout_runner(self) -> None:
        self.assertEqual(run_with_timeout(["/usr/bin/true"], 1.0), 0)
        self.assertEqual(run_with_timeout(["/bin/sleep", "1"], 0.01), 124)

    def test_window_geometry_for_full_and_narrow_ipad_scenes(self) -> None:
        source = """
        #include "src/native/platform_window.h"
        int main(void) {
            if (mr_win_surface_long_side(820, 1180, 800) != 1152) return 1;
            if (mr_win_surface_long_side(280, 612, 800) != 1752) return 2;
            mr_win_fit fit = mr_win_fit_surface_near_fill(820, 1180, 800, 1152, 8);
            if (fit.w < 819.99 || fit.h < 1179.99) return 3;
            fit = mr_win_fit_surface_near_fill(280, 612, 800, 1152, 8);
            if (fit.h >= 612) return 4;
            fit = mr_win_fit_surface_near_fill(280, 612, 800, 1752, 8);
            return fit.w >= 279.99 && fit.h >= 611.99 ? 0 : 5;
        }
        """
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "geometry"
            subprocess.run(
                [
                    "cc", "-std=c11", "-Werror", "-I", str(root),
                    "-x", "c", "-", "-lm", "-o", str(executable),
                ],
                input=source,
                text=True,
                capture_output=True,
                check=True,
            )
            subprocess.run([str(executable)], check=True)


class ArchiveTests(unittest.TestCase):
    def extract(self, entries: list[tuple[zipfile.ZipInfo | str, bytes]]) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        archive_path = root / "release.zip"
        with zipfile.ZipFile(archive_path, "w") as archive:
            for name, data in entries:
                archive.writestr(name, data)
        destination = root / "output"
        with zipfile.ZipFile(archive_path) as archive:
            extract_release_archive(archive, destination)
        return destination

    def test_extracts_regular_files(self) -> None:
        destination = self.extract([("folder/data.bin", b"game")])
        self.assertEqual((destination / "folder/data.bin").read_bytes(), b"game")

    def test_rejects_parent_traversal(self) -> None:
        with self.assertRaisesRegex(ValueError, "unsafe ZIP entry"):
            self.extract([("../outside", b"bad")])

    def test_rejects_absolute_path(self) -> None:
        with self.assertRaisesRegex(ValueError, "unsafe ZIP entry"):
            self.extract([("/outside", b"bad")])

    def test_rejects_symbolic_link(self) -> None:
        link = zipfile.ZipInfo("link")
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        with self.assertRaisesRegex(ValueError, "unsafe ZIP entry"):
            self.extract([(link, b"target")])

    def test_rejects_case_insensitive_duplicate(self) -> None:
        with self.assertRaisesRegex(ValueError, "unsafe ZIP entry"):
            self.extract([("Data/File", b"a"), ("data/file", b"b")])

    def test_validates_complete_jpk(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "content.jpk"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("content.bin", b"game-data")
            validate_jpk_archives(path.parent)

    def test_rejects_truncated_jpk(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "content.jpk"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("content.bin", b"game-data")
            path.write_bytes(path.read_bytes()[:-16])
            with self.assertRaisesRegex(ValueError, "truncated"):
                validate_jpk_archives(path.parent)

    def test_rejects_truncated_mandatory_jpk(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = (
                Path(temporary)
                / "dlc_v2_basic_mandatory_3_android_basic_3_45.jpk"
            )
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("model.pig", b"game-data")
            path.write_bytes(path.read_bytes()[:-16])
            with self.assertRaisesRegex(ValueError, "truncated"):
                validate_jpk_archives(path.parent)


class LocalizationTests(unittest.TestCase):
    SOURCE = [
        TextEntry("TITLE", "Collect %d Bananas"),
        TextEntry("BODY", "Come back in %H:%M:%S"),
    ]

    def test_binary_text_table_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "sample.texts"
            path.write_bytes(encode_texts(self.SOURCE))
            self.assertEqual(read_texts(path), self.SOURCE)

    def test_font_codes_do_not_consume_translation_text(self) -> None:
        self.assertEqual(token_signature("#F14Additional 3D Art"), {"#F14": 1})
        self.assertEqual(token_signature("#F14Translated 3D art"), {"#F14": 1})

    def test_xliff_preserves_order_and_placeholders(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "hu.xlf"
            create_xliff(self.SOURCE, "hu", path)
            tree = ET.parse(path)
            namespace = {"x": "urn:oasis:names:tc:xliff:document:2.0"}
            targets = tree.getroot().findall("x:file/x:unit/x:segment/x:target", namespace)
            segments = tree.getroot().findall("x:file/x:unit/x:segment", namespace)
            targets[0].text = "Translated %d value"
            targets[1].text = "Return in %H:%M:%S"
            for segment in segments:
                segment.set("state", "final")
            tree.write(path, encoding="utf-8", xml_declaration=True)
            self.assertEqual([entry.key for entry in read_xliff(path, self.SOURCE, "hu")],
                             ["TITLE", "BODY"])

            targets[0].text = "Wrong placeholder: %s"
            tree.write(path, encoding="utf-8", xml_declaration=True)
            with self.assertRaisesRegex(ValueError, "placeholder mismatch"):
                read_xliff(path, self.SOURCE, "hu")

            targets[0].text = "A" * 12 + " %d"
            tree.write(path, encoding="utf-8", xml_declaration=True)
            with self.assertRaisesRegex(ValueError, "repeated-letter run"):
                read_xliff(path, self.SOURCE, "hu")

            targets[0].text = "An excessively long translation. " * 3 + "%d"
            tree.write(path, encoding="utf-8", xml_declaration=True)
            with self.assertRaisesRegex(ValueError, "expands excessively"):
                read_xliff(path, self.SOURCE, "hu")

    def test_generated_flag_layout_is_data_driven(self) -> None:
        catalog = Catalog(
            code="sample",
            display_name="Sample",
            engine_code="sm",
            country="SM",
            flag_source="resources/flags/sample.svg",
            flag_layout="vertical",
            flag_colors=(0x112233, 0x445566),
            font_archive="fonts.jpk",
            font_member="font.ttf",
            formatting={},
            path=Path("sample.json"),
        )
        header = generated_header([catalog]).decode("utf-8")
        self.assertIn("MR_FLAG_BANDS_VERTICAL", header)
        self.assertIn("MR_LOCALIZATION_FLAG_SLICE_COUNT 13u", header)
        self.assertIn("MR_LOCALIZATION_FLAG_RECTANGLE_CAPACITY 39u", header)
        self.assertIn("0x112233u, 0x445566u", header)


class DeviceTests(unittest.TestCase):
    PAYLOAD = {
        "result": {
            "devices": [
                {
                    "identifier": "device-2",
                    "hardwareProperties": {
                        "reality": "physical",
                        "udid": "udid-2",
                    },
                    "connectionProperties": {"pairingState": "paired"},
                    "deviceProperties": {"name": "Zita iPad"},
                },
                {
                    "identifier": "device-1",
                    "hardwareProperties": {
                        "reality": "physical",
                        "udid": "udid-1",
                    },
                    "connectionProperties": {"pairingState": "paired"},
                    "deviceProperties": {"name": "Anna iPhone"},
                },
                {
                    "identifier": "virtual-device",
                    "hardwareProperties": {
                        "reality": "virtual",
                        "udid": "sim-1",
                    },
                    "connectionProperties": {"pairingState": "paired"},
                    "deviceProperties": {"name": "Virtual Device"},
                },
            ]
        }
    }

    def test_returns_sorted_physical_devices(self) -> None:
        devices = paired_physical_devices(self.PAYLOAD, [])
        self.assertEqual(
            devices,
            [
                ("udid-1", "device-1", "Anna iPhone"),
                ("udid-2", "device-2", "Zita iPad"),
            ],
        )

    def test_filters_by_case_insensitive_name(self) -> None:
        devices = paired_physical_devices(self.PAYLOAD, ["IPAD"])
        self.assertEqual(devices, [("udid-2", "device-2", "Zita iPad")])

    def test_selects_latest_ios_26_and_low_end_phone(self) -> None:
        runtimes = {
            "runtimes": [
                {"identifier": "ios-25", "version": "25.4", "isAvailable": True},
                {"identifier": "ios-26-4", "version": "26.4", "isAvailable": True},
                {"identifier": "ios-26-5", "version": "26.5", "isAvailable": True},
            ]
        }
        device_types = {
            "devicetypes": [
                {"name": "iPhone 11", "identifier": "phone-11"},
                {"name": "iPhone SE (2nd generation)", "identifier": "phone-se-2"},
                {"name": "iPad (A16)", "identifier": "tablet-a16"},
            ]
        }
        self.assertEqual(select(runtimes, device_types), ("ios-26-5", "phone-se-2", "tablet-a16"))

    def test_rejects_missing_device_list(self) -> None:
        with self.assertRaises(ValueError):
            paired_physical_devices({}, [])

    def test_classifies_ios_startup_log(self) -> None:
        self.assertEqual(startup_state("loading"), "pending")
        self.assertEqual(startup_state("nativeRender STOPPED: fault"), "failed")
        self.assertEqual(
            startup_state("ERROR: guest attempted a network import during startup"),
            "failed",
        )
        ready = "startup frame check: PNG saved\nstartup services ready: 8.0 s"
        self.assertEqual(startup_state(ready), "ready")

    def test_validates_startup_png_header(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "startup.png"
            path.write_bytes(
                b"\x89PNG\r\n\x1a\n" + b"\0\0\0\rIHDR" +
                (960).to_bytes(4, "big") + (1376).to_bytes(4, "big")
            )
            self.assertTrue(valid_png(path))
            self.assertTrue(valid_png(path, portrait=True))
            path.write_bytes(
                b"\x89PNG\r\n\x1a\n" + b"\0\0\0\rIHDR" +
                (1376).to_bytes(4, "big") + (960).to_bytes(4, "big")
            )
            self.assertTrue(valid_png(path))
            self.assertFalse(valid_png(path, portrait=True))
            path.write_bytes(b"not a PNG")
            self.assertFalse(valid_png(path))


if __name__ == "__main__":
    unittest.main()
