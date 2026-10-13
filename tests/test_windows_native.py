"""Portable Windows kit supply-chain and validation tests with fake inputs."""
from __future__ import annotations

import hashlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
sys.dont_write_bytecode = True

import build_windows_kit
import build_windows_onefile
from package_windows_release import package_windows_candidate
from windows_utm_transport import _utm_http_stream, _utm_webdav_url
from windows_native_package import (_needs_local_archive_copy,
                                    _stage_source_archive, create_unsigned_ipa,
                                    validate_kit)


class WindowsKitTests(unittest.TestCase):
    def validate(self, kit: Path):
        return validate_kit(kit)

    def fixture(self, root: Path, *, mode: str = "release"):
        python_zip = root / "python-embed.zip"
        with zipfile.ZipFile(python_zip, "w") as archive:
            for name in build_windows_kit.PYTHON_FILES_REQUIRED:
                archive.writestr(name, b"synthetic test fixture")
        compiler = root / "a64-compiler.exe"
        compiler.write_bytes(b"MZ" + b"\x00" * 250)
        gui = root / "MinionRush Builder.exe"
        pe = bytearray(512)
        pe[:2] = b"MZ"
        struct.pack_into("<I", pe, 60, 128)
        pe[128:132] = b"PE\0\0"
        struct.pack_into("<H", pe, 132, 0x8664)
        struct.pack_into("<H", pe, 148, 240)
        struct.pack_into("<H", pe, 152, 0x20B)
        struct.pack_into("<H", pe, 220, 2)
        if mode == "utm-test":
            pe.extend("UTM test build".encode("utf-16le"))
        gui.write_bytes(pe)
        template = root / "unsigned-template.zip"
        template.write_bytes(b"unsigned template fixture")
        output = root / "kit"
        checksum = hashlib.sha256(python_zip.read_bytes()).hexdigest()
        with mock.patch.object(build_windows_kit, "PYTHON_3147_EMBED_AMD64_SHA256", checksum):
            build_windows_kit.build_kit(python_zip, compiler, template, gui, output,
                                        mode=mode)
        return output

    def test_kit_contains_only_expected_public_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            kit = self.fixture(Path(directory))
            config = self.validate(kit)
            self.assertEqual(config["version"], 4)
            self.assertFalse((kit / "assets").exists())
            self.assertFalse((kit / "runtime/assets").exists())
            self.assertFalse((kit / "runtime/build").exists())
            self.assertTrue((kit / "python/LICENSE.txt").exists())
            self.assertTrue((kit / "MinionRush Builder.exe").is_file())
            self.assertFalse((kit / "run-native-ipa.cmd").exists())
            self.assertFalse((kit / "run-native-ipa.ps1").exists())
            self.assertEqual(
                (kit / "python/python314._pth").read_bytes(),
                b"python314.zip\n.\n..\\runtime\\tools\n",
            )
            self.assertEqual(config["mode"], "release")
            self.assertFalse((kit / "runtime/tools/windows_utm_transport.py").exists())
            self.assertFalse((kit / "python/site-packages").exists())
            self.assertTrue((kit / "python-file-hashes.json").is_file())


    def test_test_profile_contains_utm_support_and_rejects_mismatched_gui(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kit = self.fixture(root, mode="utm-test")
            self.assertEqual(self.validate(kit)["mode"], "utm-test")
            self.assertTrue((kit / "runtime/tools/windows_utm_transport.py").is_file())
            source = root / "MinionRush Builder.exe"
            test_bytes = source.read_bytes()
            source.write_bytes(test_bytes.replace("UTM test build".encode("utf-16le"),
                                                  b"Release build!!!".ljust(28, b"x")))
            with mock.patch.object(build_windows_kit, "PYTHON_3147_EMBED_AMD64_SHA256",
                                   hashlib.sha256((root / "python-embed.zip").read_bytes()).hexdigest()):
                with self.assertRaisesRegex(ValueError, "does not match"):
                    build_windows_kit.build_kit(root / "python-embed.zip",
                                                root / "a64-compiler.exe",
                                                root / "unsigned-template.zip", source,
                                                root / "other", mode="utm-test")

    def test_test_kit_requires_explicit_private_packaging_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kit = self.fixture(root, mode="utm-test")
            archive = root / "test-kit.zip"
            with self.assertRaisesRegex(ValueError, "explicit"):
                package_windows_candidate(kit, archive)
            package_windows_candidate(kit, archive, allow_test_profile=True)
            self.assertTrue(archive.is_file())

    def test_release_mode_does_not_import_utm_transport(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "original.zip"
            source.write_bytes(b"synthetic")
            scratch = root / "scratch"
            scratch.mkdir()
            with (mock.patch("windows_native_package._needs_local_archive_copy",
                             return_value=True),
                  mock.patch("windows_utm_transport._utm_webdav_url",
                             side_effect=AssertionError("UTM called in release build"))):
                staged = _stage_source_archive(source, scratch, mode="release")
            self.assertEqual(staged.read_bytes(), b"synthetic")

    def test_private_hungarian_catalog_is_verified_in_kit_and_onefile(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kit = self.fixture(root)
            private = root / "hu.xlf"
            private.write_bytes(b"private test translation")
            newkit = root / "kit-with-language"
            checksum = hashlib.sha256((root / "python-embed.zip").read_bytes()).hexdigest()
            with mock.patch.object(build_windows_kit, "PYTHON_3147_EMBED_AMD64_SHA256",
                                   checksum):
                build_windows_kit.build_kit(root / "python-embed.zip",
                                            root / "a64-compiler.exe",
                                            root / "unsigned-template.zip",
                                            root / "MinionRush Builder.exe",
                                            newkit, hungarian_xlf=private)
            self.assertEqual(self.validate(newkit)["hungarian_xlf_sha256"],
                             hashlib.sha256(private.read_bytes()).hexdigest())
            bundled = newkit / "private-localizations/hu.xlf"
            self.assertEqual(bundled.read_bytes(), private.read_bytes())
            original = bundled.read_bytes()
            bundled.write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "checksum"):
                self.validate(newkit)
            bundled.write_bytes(original)

            stub = root / "outer-launcher.exe"
            stub.write_bytes((root / "MinionRush Builder.exe").read_bytes() +
                             bytes(4096))
            output = root / "MinionRush Portable.exe"
            digest = build_windows_onefile.build_onefile(stub, newkit, output)
            self.assertEqual(digest, hashlib.sha256(output.read_bytes()).hexdigest())
            with output.open("rb") as source:
                source.seek(-build_windows_onefile.FOOTER.size, 2)
                magic, offset, size, expected = build_windows_onefile.FOOTER.unpack(
                    source.read(build_windows_onefile.FOOTER.size))
                self.assertEqual(magic, build_windows_onefile.BUNDLE_MAGIC)
                source.seek(offset)
                archive = source.read(size)
            self.assertEqual(hashlib.sha256(archive).digest(), expected)
            with zipfile.ZipFile(io.BytesIO(archive)) as zipped:
                self.assertEqual(zipped.read(
                    "MinionRush-Portable-Windows/private-localizations/hu.xlf"),
                    private.read_bytes())
            self.assertFalse(output.with_name(output.name + ".partial").exists())

    def test_packaged_gui_release_contains_no_private_game_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kit = self.fixture(root)
            output = root / "Windows-GUI-candidate.zip"
            digest = package_windows_candidate(kit, output)
            self.assertEqual(hashlib.sha256(output.read_bytes()).hexdigest(), digest)
            with zipfile.ZipFile(output) as archive:
                self.assertIsNone(archive.testzip())
                contents = archive.namelist()
                self.assertIn(
                    "MinionRush-Portable-Windows/MinionRush Builder.exe", contents)
                self.assertNotIn(
                    "MinionRush-Portable-Windows/original-1.8.1g-PRIVATE.zip", contents)
                self.assertFalse(any(name.endswith((".ipa", ".xlf", ".so"))
                                     for name in contents))
            with self.assertRaisesRegex(ValueError, "new .zip"):
                package_windows_candidate(kit, output)

    def test_rejects_changed_template_compiler_and_runtime_file(self):
        with tempfile.TemporaryDirectory() as directory:
            kit = self.fixture(Path(directory))
            for path, data in (
                (kit / "MinionRush-native-template-unsigned.zip", b"corrupted"),
                (kit / "a64-compiler.exe", b"corrupted"),
                (kit / "MinionRush Builder.exe", b"corrupted"),
                (kit / "runtime/tools/native_ipa.py", b"corrupted"),
            ):
                original = path.read_bytes()
                path.write_bytes(data)
                try:
                    with self.assertRaisesRegex(ValueError, "checksum|integrity"):
                        self.validate(kit)
                finally:
                    path.write_bytes(original)
            self.assertEqual(self.validate(kit)["version"], 4)
            tamper = kit / "python/python314._pth"
            original = tamper.read_bytes()
            tamper.write_bytes(b"import site\n")
            with self.assertRaisesRegex(ValueError, "integrity"):
                self.validate(kit)
            tamper.write_bytes(original)
            self.assertEqual(self.validate(kit)["version"], 4)
            (kit / "runtime/tools/untrusted.py").write_text("print('bad')")
            with self.assertRaisesRegex(ValueError, "extra files"):
                self.validate(kit)


    def test_remote_archive_is_staged_for_seekable_local_zip_reads(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "original.zip"
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("synthetic-test.txt", b"private test bytes")
            target = root / "working"
            target.mkdir()
            with mock.patch("windows_native_package._needs_local_archive_copy",
                            return_value=True):
                copied = _stage_source_archive(archive, target)
            self.assertEqual(copied, target / "original-private-source.zip")
            self.assertEqual(copied.read_bytes(), archive.read_bytes())
            with zipfile.ZipFile(copied) as opened:
                self.assertEqual(opened.read("synthetic-test.txt"), b"private test bytes")
            self.assertEqual(len(list(target.iterdir())), 1)

    def test_utm_local_http_copy_preserves_private_zip_and_bypasses_share_open(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "original.zip"
            source.write_bytes(b"unchanged private source")
            scratch = root / "scratch"
            scratch.mkdir()
            data = b"synthetic guest-local HTTP contents" * 4000
            stream = io.BytesIO(data)
            stream.headers = {"Content-Length": str(len(data))}
            with (mock.patch("windows_native_package._needs_local_archive_copy",
                             return_value=True),
                  mock.patch("windows_utm_transport._utm_webdav_url",
                             return_value="http://127.0.0.1:9843/original.zip"),
                  mock.patch("windows_utm_transport._utm_http_stream",
                             return_value=stream)):
                staged = _stage_source_archive(source, scratch, mode="utm-test")
            self.assertEqual(staged.read_bytes(), data)
            self.assertEqual(source.read_bytes(), b"unchanged private source")

    def test_utm_local_http_rejects_truncated_transfer(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "original.zip"
            source.write_bytes(b"source")
            scratch = root / "scratch"
            scratch.mkdir()
            stream = io.BytesIO(b"only part of ZIP")
            stream.headers = {"Content-Length": "1000"}
            with (mock.patch("windows_native_package._needs_local_archive_copy",
                             return_value=True),
                  mock.patch("windows_utm_transport._utm_webdav_url",
                             return_value="http://127.0.0.1:9843/original.zip"),
                  mock.patch("windows_utm_transport._utm_http_stream",
                             return_value=stream)):
                with self.assertRaisesRegex(ValueError, "incomplete"):
                    _stage_source_archive(source, scratch, mode="utm-test")

    def test_utm_local_http_rejects_external_hosts_and_oversized_files(self):
        with self.assertRaisesRegex(ValueError, "unsafe UTM share URL"):
            _utm_http_stream("http://example.com/original.zip")
        stream = io.BytesIO(b"")
        stream.headers = {"Content-Length": str(3 * 1024 ** 3)}
        opener = mock.MagicMock()
        opener.open.return_value = stream
        with mock.patch("windows_utm_transport.urllib.request.build_opener",
                        return_value=opener):
            with self.assertRaisesRegex(ValueError, "exceeds supported size"):
                _utm_http_stream("http://127.0.0.1:9843/original.zip")
        self.assertTrue(stream.closed)

    def test_local_zip_skips_unnecessary_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "original.zip"
            archive.write_bytes(b"not a real original archive")
            self.assertIs(_stage_source_archive(archive, root), archive)
            self.assertEqual(len(list(root.iterdir())), 1)

    def test_staged_zip_has_strict_bound_and_stays_outside_kit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "original.zip"
            source.write_bytes(b"123456789")
            scratch = root / "scratch"
            scratch.mkdir()
            with (mock.patch("windows_native_package._needs_local_archive_copy",
                             return_value=True),
                  mock.patch("windows_native_package.MAX_ARCHIVE_BYTES", 8)):
                with self.assertRaisesRegex(ValueError, "exceeds"):
                    _stage_source_archive(source, scratch)
            self.assertEqual(source.read_bytes(), b"123456789")
            self.assertFalse((root / "kit").exists())

    def test_full_builder_uses_staged_zip_and_cleans_up(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            private_zip = root / "original.zip"
            private_zip.write_bytes(b"synthetic private source zip")
            kit = root / "kit"
            kit.mkdir()
            private_translations = root / "MinionRush-Private-Languages"
            private_translations.mkdir()
            (private_translations / "hu.xlf").write_text("synthetic private catalog")
            output = root / "final.ipa"
            def verify_prepare(path, output_path, **kwargs):
                self.assertEqual(path.name, "original-private-source.zip")
                self.assertEqual(path.read_bytes(), private_zip.read_bytes())
                self.assertNotEqual(path.parent, kit)
                self.assertEqual(kwargs["translations_root"].resolve(), private_translations.resolve())
            def verify_assemble(template, private, dest):
                dest.write_bytes(b"synthetic unsigned IPA")
            with (mock.patch("windows_native_package.validate_kit",
                             return_value={"compiler_sha256": "0" * 64}),
                  mock.patch("windows_native_package._needs_local_archive_copy",
                             return_value=True),
                  mock.patch("windows_native_package.prepare", verify_prepare),
                  mock.patch("windows_native_package.assemble_unsigned_ipa", verify_assemble)):
                create_unsigned_ipa(private_zip, kit, output)
            self.assertEqual(output.read_bytes(), b"synthetic unsigned IPA")
            self.assertEqual(private_zip.read_bytes(), b"synthetic private source zip")
            self.assertFalse(list(root.glob("minion-rush-private-*")))

    @unittest.skipUnless(os.name == "nt", "Windows-specific UTM drive mapping")
    def test_utm_mapped_drive_and_unc_url_resolution(self):
        from pathlib import PureWindowsPath
        unc = PureWindowsPath(r"\\localhost@9843\DavWWWRoot\original-1.8.1g-PRIVATE.zip")
        self.assertTrue(_needs_local_archive_copy(unc))
        self.assertEqual(_utm_webdav_url(unc),
                         "http://127.0.0.1:9843/original-1.8.1g-PRIVATE.zip")
        with mock.patch("windows_utm_transport._mapped_unc_root",
                        return_value=r"\\localhost@9843\DavWWWRoot"):
            self.assertEqual(
                _utm_webdav_url(PureWindowsPath(r"Z:\original-1.8.1g-PRIVATE.zip")),
                "http://127.0.0.1:9843/original-1.8.1g-PRIVATE.zip",
            )

    def test_embedded_hungarian_checkbox_controls_ipa_contents(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "original.zip"
            source.write_bytes(b"synthetic source")
            kit = root / "kit"
            kit.mkdir()
            embedded = kit / "private-localizations"
            embedded.mkdir()
            (embedded / "hu.xlf").write_bytes(b"synthetic private Hungarian catalog")
            output = root / "result.ipa"
            state = []

            def capture(archive, destination, **kwargs):
                state.append((kwargs["translations_root"],
                              kwargs["enable_localizations"]))
                self.assertEqual(archive, source)

            def assemble(template, folder, destination):
                destination.write_bytes(b"unsigned synthetic IPA")

            with (mock.patch("windows_native_package.validate_kit",
                             return_value={"mode": "release", "compiler_sha256": "0" * 64,
                                           "hungarian_xlf_sha256": "a" * 64}),
                  mock.patch("windows_native_package.prepare", capture),
                  mock.patch("windows_native_package.assemble_unsigned_ipa", assemble)):
                create_unsigned_ipa(source, kit, output, include_hungarian=True)
                self.assertEqual(state[-1], (embedded.resolve(), True))
                output.unlink()
                create_unsigned_ipa(source, kit, output, include_hungarian=False)
                self.assertEqual(state[-1], (None, False))
                with self.assertRaisesRegex(ValueError, "conflicts"):
                    create_unsigned_ipa(source, kit, root / "other.ipa",
                                        hungarian_xlf=embedded / "hu.xlf",
                                        include_hungarian=False)
            self.assertEqual(output.read_bytes(), b"unsigned synthetic IPA")

    def test_existing_ipa_requires_explicit_overwrite_and_replaces_atomically(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "original.zip"
            source.write_bytes(b"synthetic archive")
            kit = root / "kit"
            kit.mkdir()
            target = root / "existing.ipa"
            target.write_bytes(b"previous signed or unsigned IPA")
            calls = []

            def assemble(template, folder, destination):
                self.assertTrue(target.is_file())
                self.assertEqual(target.read_bytes(), b"previous signed or unsigned IPA")
                self.assertNotEqual(destination, target)
                self.assertEqual(destination.parent.parent, root)
                calls.append(destination)
                destination.write_bytes(b"new unsigned IPA")

            with (mock.patch("windows_native_package.validate_kit",
                             return_value={"compiler_sha256": "0" * 64}),
                  mock.patch("windows_native_package.prepare"),
                  mock.patch("windows_native_package.assemble_unsigned_ipa", assemble)):
                with self.assertRaisesRegex(ValueError, "without approval"):
                    create_unsigned_ipa(source, kit, target)
                self.assertEqual(target.read_bytes(), b"previous signed or unsigned IPA")
                self.assertEqual(calls, [])
                create_unsigned_ipa(source, kit, target, overwrite=True)

            self.assertEqual(len(calls), 1)
            self.assertEqual(target.read_bytes(), b"new unsigned IPA")
            self.assertFalse(calls[0].exists())
            self.assertFalse(any(p.name.startswith(".mr-replace-") for p in root.iterdir()))

    def test_failed_overwrite_preserves_original_and_cleans_staged_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "original.zip"
            source.write_bytes(b"synthetic archive")
            kit = root / "kit"
            kit.mkdir()
            target = root / "existing.ipa"
            original = b"working old IPA"
            target.write_bytes(original)

            def fail_after_write(template, folder, destination):
                destination.write_bytes(b"partial new IPA")
                raise RuntimeError("synthetic assembly failure")

            with (mock.patch("windows_native_package.validate_kit",
                             return_value={"compiler_sha256": "0" * 64}),
                  mock.patch("windows_native_package.prepare"),
                  mock.patch("windows_native_package.assemble_unsigned_ipa",
                             fail_after_write)):
                with self.assertRaisesRegex(RuntimeError, "synthetic assembly failure"):
                    create_unsigned_ipa(source, kit, target, overwrite=True)

            self.assertEqual(target.read_bytes(), original)
            self.assertFalse(any(p.name.startswith(".mr-replace-") for p in root.iterdir()))

    def test_overwrite_rejects_changed_or_disappearing_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "original.zip"
            source.write_bytes(b"synthetic archive")
            kit = root / "kit"
            kit.mkdir()
            target = root / "existing.ipa"
            original = b"working old IPA"
            target.write_bytes(original)

            def modified_externally(template, folder, destination):
                destination.write_bytes(b"new unsigned IPA")
                target.write_bytes(b"different outside edit")

            with (mock.patch("windows_native_package.validate_kit",
                             return_value={"compiler_sha256": "0" * 64}),
                  mock.patch("windows_native_package.prepare"),
                  mock.patch("windows_native_package.assemble_unsigned_ipa",
                             modified_externally)):
                with self.assertRaisesRegex(ValueError, "changed during build"):
                    create_unsigned_ipa(source, kit, target, overwrite=True)
            self.assertEqual(target.read_bytes(), b"different outside edit")
            self.assertFalse(any(p.name.startswith(".mr-replace-") for p in root.iterdir()))
            target.unlink()
            with self.assertRaisesRegex(ValueError, "has disappeared"):
                create_unsigned_ipa(source, kit, target, overwrite=True)

    def test_overwrite_rejects_linked_or_directory_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "original.zip"
            source.write_bytes(b"synthetic archive")
            kit = root / "kit"
            kit.mkdir()
            directory_target = root / "folder.ipa"
            directory_target.mkdir()
            with self.assertRaisesRegex(ValueError, "without approval"):
                create_unsigned_ipa(source, kit, directory_target, overwrite=True)
            target = root / "real.ipa"
            target.write_bytes(b"original")
            link = root / "linked.ipa"
            try:
                link.symlink_to(target)
            except OSError:
                pass
            else:
                with self.assertRaisesRegex(ValueError, "linked output"):
                    create_unsigned_ipa(source, kit, link, overwrite=True)
                self.assertEqual(target.read_bytes(), b"original")

    def test_builder_allows_original_zip_without_optional_language(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "original.zip"
            source.write_bytes(b"synthetic source")
            kit = root / "kit"
            kit.mkdir()
            output = root / "result.ipa"
            def capture(archive, destination, **kwargs):
                self.assertIsNone(kwargs["translations_root"])
            def assemble(template, folder, destination):
                destination.write_bytes(b"unsigned synthetic IPA")
            with (mock.patch("windows_native_package.validate_kit",
                             return_value={"compiler_sha256": "0" * 64}),
                  mock.patch("windows_native_package.prepare", capture),
                  mock.patch("windows_native_package.assemble_unsigned_ipa", assemble)):
                create_unsigned_ipa(source, kit, output)
            self.assertEqual(output.read_bytes(), b"unsigned synthetic IPA")

    def test_builder_places_temporary_data_in_selected_scratch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "original.zip"
            source.write_bytes(b"private archive fixture")
            kit = root / "kit"
            kit.mkdir()
            scratch = root / "scratch"
            scratch.mkdir()
            output = root / "result.ipa"
            def check_prepare(zip_path, output_path, **kwargs):
                self.assertEqual(output_path.parent.parent, scratch)
            def assemble(template, private, outfile):
                outfile.write_bytes(b"unsigned fixture")
            with (mock.patch("windows_native_package.validate_kit",
                             return_value={"compiler_sha256": "0" * 64}),
                  mock.patch("windows_native_package.prepare", check_prepare),
                  mock.patch("windows_native_package.assemble_unsigned_ipa", assemble)):
                create_unsigned_ipa(source, kit, output, scratch_root=scratch)
            self.assertEqual(output.read_bytes(), b"unsigned fixture")
            self.assertEqual(list(scratch.iterdir()), [])

    def test_builder_retains_private_files_on_gui_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "original.zip"
            source.write_bytes(b"synthetic test bytes")
            kit = root / "kit"
            kit.mkdir()
            scratch = root / "scratch"
            scratch.mkdir()
            output = root / "failed.ipa"

            def fail_after_staging(archive, destination, **kwargs):
                self.assertEqual(archive.name, "original-private-source.zip")
                self.assertEqual(archive.read_bytes(), source.read_bytes())
                raise FileNotFoundError("synthetic missing file")

            with (mock.patch("windows_native_package.validate_kit",
                             return_value={"compiler_sha256": "0" * 64}),
                  mock.patch("windows_native_package._needs_local_archive_copy",
                             return_value=True),
                  mock.patch("windows_native_package.prepare", fail_after_staging)):
                with self.assertRaisesRegex(FileNotFoundError, "synthetic missing"):
                    create_unsigned_ipa(source, kit, output, scratch_root=scratch)

            folders = list(scratch.iterdir())
            self.assertEqual(len(folders), 1)
            self.assertEqual((folders[0] / "original-private-source.zip").read_bytes(),
                             source.read_bytes())
            self.assertFalse(output.exists())

    def test_builder_cleans_private_files_on_cli_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "original.zip"
            source.write_bytes(b"synthetic test bytes")
            kit = root / "kit"
            kit.mkdir()
            created = []
            def fail_and_capture(archive, destination, **kwargs):
                created.append(destination.parent)
                raise FileNotFoundError("synthetic missing")
            with (mock.patch("windows_native_package.validate_kit",
                             return_value={"compiler_sha256": "0" * 64}),
                  mock.patch("windows_native_package.prepare", fail_and_capture)):
                with self.assertRaises(FileNotFoundError):
                    create_unsigned_ipa(source, kit, root / "failed.ipa")
            self.assertEqual(len(created), 1)
            self.assertFalse(created[0].exists())

    def test_builder_rejects_untrusted_translation_filename(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "original.zip"
            archive.write_bytes(b"original")
            selected = root / "not-hu.xlf"
            selected.write_text("invalid selection")
            kit = root / "kit"
            kit.mkdir()
            with mock.patch("windows_native_package.validate_kit",
                            return_value={"compiler_sha256": "0" * 64}):
                with self.assertRaisesRegex(ValueError, "private, regular hu.xlf"):
                    create_unsigned_ipa(archive, kit, root / "result.ipa",
                                        hungarian_xlf=selected)

    def test_rejects_unverified_python_distribution(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "fake.zip"
            archive.write_bytes(b"untrusted")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                build_windows_kit._unpack_verified_python(archive, root / "python")


if __name__ == "__main__":
    unittest.main()
