from __future__ import annotations

import contextlib
import copy
import io
import json
import plistlib
import signal
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
sys.dont_write_bytecode = True

import installer_backend as backend
import install_assets
from list_ios_devices import paired_physical_devices
import test_tools
import validate_installer
from validate_assets import AssetReport


class InstallerPackageTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.root = self.directory / "source"
        names = ["README.md", "config/installer_ui.json", "config/source_manifest.txt",
                 "installer/Info.plist"]
        for name in names + ["docs/INSTALLER_DOWNLOAD.md", "LICENSE"]:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(TOOLS.parent / name, path)
        (self.root / "config/source_manifest.txt").write_text("\n".join(names) + "\n")
        self.app = self.directory / "Minion Rush Installer.app"
        files = {"Contents/Info.plist": self.root / "installer/Info.plist",
                 "Contents/Resources/installer_ui.json": self.root / "config/installer_ui.json"}
        files.update({"Contents/Resources/Runtime/" + name: self.root / name for name in names})
        for name, source in files.items():
            path = self.app / name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, path)
        for name, data in {"Contents/MacOS/MinionRushInstaller":
                           struct.pack("<II", 0xFEEDFACF, 0x0100000C),
                           "Contents/Resources/Installer.icns": b"icns",
                           "Contents/_CodeSignature/CodeResources": b"fixture"}.items():
            path = self.app / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.executable = self.app / "Contents/MacOS/MinionRushInstaller"
        self.executable.chmod(0o755)

    def archive(self, *, changed: str = "", omitted: str = "", executable_mode: int = 0o755) -> Path:
        archive = self.directory / "download.zip"
        files = {f"{self.app.stem}/{self.app.name}/{name}": path
                 for name, path in validate_installer.bundle_files(self.root, self.app).items()}
        files.update({f"{self.app.stem}/README.md": self.root / "docs/INSTALLER_DOWNLOAD.md",
                      f"{self.app.stem}/LICENSE": self.root / "LICENSE"})
        with zipfile.ZipFile(archive, "w") as output:
            for name, path in files.items():
                if name == omitted:
                    continue
                entry = zipfile.ZipInfo(name)
                entry.create_system = 3
                mode = executable_mode if path == self.executable else 0o644
                entry.external_attr = (stat.S_IFREG | mode) << 16
                output.writestr(entry, b"changed" if name == changed else path.read_bytes())
        return archive

    def test_public_bundle_and_archive_match(self) -> None:
        self.assertEqual(len(validate_installer.bundle_files(self.root, self.app)), 9)
        validate_installer.validate_archive(self.root, self.app, self.archive())

    def test_private_bundle_files_and_empty_directories_are_rejected(self) -> None:
        path = self.app / "Contents/Resources/assets"
        path.mkdir()
        with self.assertRaisesRegex(ValueError, "directories"):
            validate_installer.bundle_files(self.root, self.app)
        (path / "private.bin").write_bytes(b"private")
        with self.assertRaisesRegex(ValueError, "files"):
            validate_installer.bundle_files(self.root, self.app)

    def test_changed_public_source_is_rejected(self) -> None:
        (self.app / "Contents/Resources/Runtime/README.md").write_text("changed")
        with self.assertRaisesRegex(ValueError, "source differs"):
            validate_installer.bundle_files(self.root, self.app)

    def test_bundle_links_are_rejected(self) -> None:
        (self.app / "Contents/Resources/linked").symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "Links"):
            validate_installer.bundle_files(self.root, self.app)

    def test_missing_bundle_file_is_rejected(self) -> None:
        self.executable.unlink()
        with self.assertRaisesRegex(ValueError, "files"):
            validate_installer.bundle_files(self.root, self.app)

    def test_executable_architecture_and_permissions_are_required(self) -> None:
        self.executable.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "permissions"):
            validate_installer.bundle_files(self.root, self.app)
        self.executable.chmod(0o755)
        self.executable.write_bytes(struct.pack("<II", 0xFEEDFACF, 0x01000007))
        with self.assertRaisesRegex(ValueError, "ARM"):
            validate_installer.bundle_files(self.root, self.app)

    def test_changed_and_missing_archive_content_is_rejected(self) -> None:
        name = f"{self.app.stem}/README.md"
        with self.assertRaisesRegex(ValueError, "content differs"):
            validate_installer.validate_archive(self.root, self.app, self.archive(changed=name))
        with self.assertRaisesRegex(ValueError, "missing"):
            validate_installer.validate_archive(self.root, self.app, self.archive(omitted=name))

    def test_archive_rejects_private_linked_and_duplicate_entries(self) -> None:
        for name, mode in (("assets/private.bin", stat.S_IFREG),
                           ("linked", stat.S_IFLNK),
                           (f"{self.app.stem}/LICENSE", stat.S_IFREG)):
            with self.subTest(name=name):
                archive = self.archive()
                with zipfile.ZipFile(archive, "a") as output, mock.patch("warnings.warn"):
                    entry = zipfile.ZipInfo(name)
                    entry.external_attr = mode << 16
                    output.writestr(entry, b"private")
                with self.assertRaises(ValueError):
                    validate_installer.validate_archive(self.root, self.app, archive)

    def test_archive_requires_executable_permissions(self) -> None:
        with self.assertRaisesRegex(ValueError, "permissions"):
            validate_installer.validate_archive(self.root, self.app, self.archive(executable_mode=0o644))

    def test_truncated_archive_is_rejected(self) -> None:
        archive = self.archive()
        archive.write_bytes(archive.read_bytes()[:-30])
        with self.assertRaises(zipfile.BadZipFile):
            validate_installer.validate_archive(self.root, self.app, archive)


class AssetTransactionTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.root = self.directory / "runtime"
        self.root.mkdir()
        self.assets = self.directory / "assets"
        self.assets.mkdir()
        (self.assets / "old").write_bytes(b"previous assets")
        self.archive = self.directory / "release.zip"

    def stage(self, source: Path, root: Path, assets: Path) -> None:
        assets.mkdir()
        (assets / "new").write_bytes(b"validated assets")

    def test_invalid_archive_preserves_existing_assets(self) -> None:
        self.archive.write_bytes(b"not a ZIP")
        with self.assertRaises(zipfile.BadZipFile):
            install_assets.install_release(self.archive, self.root, self.assets)
        self.assertEqual((self.assets / "old").read_bytes(), b"previous assets")
        self.assertEqual(sorted(p.name for p in self.directory.iterdir()),
                         ["assets", "release.zip", "runtime"])

    def test_validation_failure_does_not_commit(self) -> None:
        with mock.patch.object(install_assets, "populate_assets", side_effect=self.stage), \
             mock.patch.object(install_assets, "inspect_assets", side_effect=ValueError("damaged")):
            with self.assertRaisesRegex(ValueError, "damaged"):
                install_assets.install_release(self.root, self.root, self.assets)
        self.assertTrue((self.assets / "old").exists())
        self.assertFalse((self.assets / "new").exists())

    def test_success_replaces_instead_of_merging(self) -> None:
        report = AssetReport(1, 16, 0)
        with mock.patch.object(install_assets, "populate_assets", side_effect=self.stage), \
             mock.patch.object(install_assets, "inspect_assets", return_value=report):
            actual = install_assets.install_release(self.root, self.root, self.assets)
        self.assertEqual(actual, report)
        self.assertFalse((self.assets / "old").exists())
        self.assertTrue((self.assets / "new").exists())

    def test_commit_failure_restores_previous_directory(self) -> None:
        staged = self.directory / "staged"
        backup = self.directory / "backup"
        with self.assertRaises(FileNotFoundError):
            install_assets.replace_assets(staged, self.assets, backup)
        self.assertTrue((self.assets / "old").exists())
        self.assertFalse(backup.exists())

    def test_interrupted_import_preserves_previous_directory(self) -> None:
        with mock.patch.object(install_assets, "populate_assets", side_effect=InterruptedError):
            with self.assertRaises(InterruptedError):
                install_assets.install_release(self.root, self.root, self.assets)
        self.assertTrue((self.assets / "old").exists())

    def test_destination_symbolic_link_is_rejected(self) -> None:
        link = self.directory / "linked-assets"
        link.symlink_to(self.assets, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "unsafe"):
            install_assets.install_release(self.root, self.root, link)
        self.assertTrue((self.assets / "old").exists())

    def test_destination_cannot_replace_the_source_root(self) -> None:
        with self.assertRaisesRegex(ValueError, "unsafe"):
            install_assets.install_release(self.root, self.root, self.root)
        self.assertTrue(self.root.is_dir())


class InstallerBackendTests(unittest.TestCase):
    def test_public_source_manifest_excludes_private_files(self) -> None:
        files = backend.public_files(TOOLS.parent)
        self.assertTrue(files)
        for file in files:
            relative = file.relative_to(TOOLS.parent).parts
            self.assertNotIn(relative[0], ("assets", "build", "localizations", ".git"))

    def test_runtime_copy_reuses_identical_sources(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            first = backend.prepare_runtime(TOOLS.parent, workspace)
            second = backend.prepare_runtime(TOOLS.parent, workspace)
            self.assertEqual(first, second)
            self.assertTrue((first / "tools/deploy_ios.sh").exists())
            self.assertFalse((first / "assets").exists())

    def test_runtime_copy_rejects_linked_parent(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            other = workspace / "other"
            other.mkdir()
            (workspace / "runtimes").symlink_to(other, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "symbolic link"):
                backend.prepare_runtime(TOOLS.parent, workspace)
            self.assertEqual(list(other.iterdir()), [])

    def test_workspace_lock_rejects_concurrent_operations(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            with backend.workspace_lock(workspace):
                with self.assertRaisesRegex(ValueError, "Another"):
                    with backend.workspace_lock(workspace):
                        self.fail("Concurrent lock acquired")
            with backend.workspace_lock(workspace):
                pass

    def test_json_logs_do_not_reenter_redirected_stdout(self) -> None:
        output = io.StringIO()
        with mock.patch.object(backend, "PROTOCOL_OUTPUT", output), \
             contextlib.redirect_stdout(backend.LogStream()):
            print("Imported engine")
        message = json.loads(output.getvalue())
        self.assertEqual(message, {"protocol": 1, "type": "log", "message": "Imported engine"})

    def test_complete_operation_log_is_private_and_kept_after_completion(self) -> None:
        output = io.StringIO()
        transcript = "build output\n" * 20_000
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            with mock.patch.object(backend, "PROTOCOL_OUTPUT", output):
                with backend.operation_log(workspace):
                    backend.emit("log", message=transcript)
                    backend.emit("error", message="Installation rejected")
                with backend.operation_log(workspace):
                    backend.emit("cancelled", message="Operation cancelled.")
            events = [json.loads(line) for line in output.getvalue().splitlines()]
            files = [Path(event["log_path"]) for event in events if event["type"] == "session"]
            self.assertEqual(len(set(files)), 2)
            self.assertEqual(files[0].read_text(), transcript + "\n[error] Installation rejected\n")
            self.assertIn("Operation cancelled.", files[1].read_text())
            self.assertEqual(stat.S_IMODE(files[0].stat().st_mode), 0o600)
        self.assertIsNone(backend.OPERATION_LOG)

    def test_operation_log_rejects_linked_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            other = workspace / "other"
            other.mkdir()
            (workspace / "logs").symlink_to(other, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "symbolic link"):
                with backend.operation_log(workspace):
                    self.fail("Linked log directory accepted")
            self.assertEqual(list(other.iterdir()), [])

    def test_shared_shell_log_preserves_stderr_and_failure_status(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "build.log"
            command = ('set -o pipefail; source "$1"; '
                       'project_run_logged "$2" /bin/bash -c '
                       '\'printf "stdout\\n"; printf "stderr\\n" >&2; exit 7\'')
            result = subprocess.run(
                ["/bin/bash", "-c", command, "test", str(TOOLS / "project_common.sh"), str(log)],
                text=True, capture_output=True, timeout=10,
            )
            self.assertEqual(result.returncode, 7)
            self.assertEqual(result.stdout, "stdout\nstderr\n")
            self.assertEqual(log.read_text(), result.stdout)

    def test_signing_settings_reject_command_injection(self) -> None:
        for team, bundle in (("bad", "org.example.game"),
                             ("ABCDEFGHIJ", "org.example;touch /tmp/invalid"),
                             ("ABCDEFGHIJ", "org..example")):
            with self.subTest(team=team, bundle=bundle), self.assertRaises(ValueError):
                backend.signing_settings(team, bundle)
        self.assertEqual(backend.signing_settings("ABCDEFGHIJ", "org.example.game"),
                         {"MR_DEVELOPMENT_TEAM": "ABCDEFGHIJ", "MR_BUNDLE_ID": "org.example.game"})

    def test_signing_settings_require_a_full_match(self) -> None:
        for team, bundle in (("ABCDEFGHIJ\n", "org.example.game"),
                             ("ABCDEFGHIJ", "org.example.game\n"),
                             ("ABCDEFGHIJ", "org.example.game ")):
            with self.subTest(team=team, bundle=bundle), self.assertRaises(ValueError):
                backend.signing_settings(team, bundle)
        self.assertEqual(backend.signing_settings("", "org.example.game", require_team=False),
                         {"MR_BUNDLE_ID": "org.example.game"})

    def test_shell_signing_uses_the_shared_rules(self) -> None:
        command = 'set -Eeuo pipefail; source "$1"; project_configure_ios_signing'
        import os
        for bundle, status in (("org.example.game", 0), ("org..example", 2)):
            environment = {**os.environ, "PYTHON": sys.executable,
                           "MR_BUNDLE_ID": bundle, "MR_DEVELOPMENT_TEAM": "ABCDEFGHIJ"}
            result = subprocess.run(
                ["/bin/bash", "-c", command, "test", str(TOOLS / "project_common.sh")],
                env=environment, text=True, capture_output=True, timeout=10,
            )
            with self.subTest(bundle=bundle):
                self.assertEqual(result.returncode, status)

    def test_environment_drops_foreign_runtime_configuration(self) -> None:
        with mock.patch.dict("os.environ", {"MR_FOREIGN_SETTING": "0.1", "BASH_ENV": "/untrusted"}):
            environment = backend.command_environment("/developer")
        self.assertNotIn("MR_FOREIGN_SETTING", environment)
        self.assertNotIn("BASH_ENV", environment)
        self.assertEqual(environment["DEVELOPER_DIR"], "/developer")

    def test_selects_exact_device_not_a_similar_name(self) -> None:
        devices = paired_physical_devices(test_tools.DeviceTests.PAYLOAD, [], "device-2")
        self.assertEqual([device["id"] for device in devices], ["device-2"])
        self.assertEqual(paired_physical_devices(test_tools.DeviceTests.PAYLOAD, [], "device"), [])

    def test_rejects_unpaired_old_and_non_ios_devices(self) -> None:
        for field, value in (("pairingState", "unpaired"), ("deviceType", "AppleTV"),
                             ("osVersionNumber", {"stringValue": "16.6"})):
            payload = copy.deepcopy(test_tools.DeviceTests.PAYLOAD)
            properties = payload["result"]["devices"][0]["properties"]
            section = {"pairingState": "connection", "deviceType": "hardware",
                       "osVersionNumber": "software"}[field]
            properties[section][field] = value
            with self.subTest(field=field):
                self.assertEqual(paired_physical_devices(payload, [], "device-2"), [])

    def test_disconnected_selection_does_not_start_installation(self) -> None:
        with mock.patch.object(backend, "query_devices", return_value=[]), \
             mock.patch.object(backend.subprocess, "Popen") as launch:
            with self.assertRaisesRegex(ValueError, "no longer"):
                backend.deploy(Path("/runtime"), Path("/workspace"), "missing", {})
            launch.assert_not_called()

    def test_deployment_uses_existing_script_and_exact_identifier(self) -> None:
        process = mock.Mock()
        process.stdout = io.StringIO("== packaging ==\ncomplete\n")
        process.wait.return_value = 0
        process.poll.return_value = 0
        environment = {}
        with mock.patch.object(backend, "query_devices", return_value=[{"id": "selected"}]), \
             mock.patch.object(backend.subprocess, "Popen", return_value=process) as launch, \
             mock.patch.object(backend, "PROTOCOL_OUTPUT", io.StringIO()):
            backend.deploy(Path("/runtime"), Path("/workspace"), "selected", environment)
        arguments, options = launch.call_args
        self.assertEqual(arguments[0], ["/bin/bash", "/runtime/tools/deploy_ios.sh",
                                       "--identifier", "selected"])
        self.assertIs(options["start_new_session"], True)
        self.assertEqual(environment["MR_ASSETS_ROOT"], "/workspace/assets")

    def test_cancelled_deployment_terminates_child_process_group(self) -> None:
        process = mock.Mock()
        process.pid = 123
        process.stdout = mock.MagicMock()
        process.stdout.__iter__.side_effect = InterruptedError
        process.poll.return_value = None
        with mock.patch.object(backend, "query_devices", return_value=[{"id": "selected"}]), \
             mock.patch.object(backend.subprocess, "Popen", return_value=process), \
             mock.patch.object(backend.os, "killpg") as terminate:
            with self.assertRaises(InterruptedError):
                backend.deploy(Path("/runtime"), Path("/workspace"), "selected", {})
        self.assertEqual(terminate.call_args_list,
                         [mock.call(123, signal.SIGTERM), mock.call(123, signal.SIGKILL)])
        self.assertEqual(process.wait.call_args_list, [mock.call(timeout=10), mock.call()])

    def test_cancellation_cleans_group_even_after_leader_exit(self) -> None:
        for wait_failure, signal_failure in ((None, None),
                                             (subprocess.TimeoutExpired("deploy", 10), None),
                                             (None, ProcessLookupError())):
            process = mock.Mock(pid=123)
            process.stdout = mock.MagicMock()
            process.stdout.__iter__.side_effect = InterruptedError
            process.poll.return_value = 0
            process.wait.side_effect = [wait_failure, 0]
            with self.subTest(wait_failure=wait_failure, signal_failure=signal_failure), \
                 mock.patch.object(backend, "query_devices", return_value=[{"id": "selected"}]), \
                 mock.patch.object(backend.subprocess, "Popen", return_value=process), \
                 mock.patch.object(backend.os, "killpg", side_effect=signal_failure) as terminate:
                with self.assertRaises(InterruptedError):
                    backend.deploy(Path("/runtime"), Path("/workspace"), "selected", {})
            self.assertEqual(terminate.call_args_list,
                             [mock.call(123, signal.SIGTERM), mock.call(123, signal.SIGKILL)])
            process.stdout.close.assert_called_once()

    def test_free_signing_limit_is_reported_without_hiding_apple_error(self) -> None:
        process = mock.Mock()
        message = "This device has reached the maximum number of installed apps using a free developer profile"
        process.stdout = io.StringIO(message + "\n")
        process.wait.return_value = 1
        process.poll.return_value = 1
        output = io.StringIO()
        with mock.patch.object(backend, "query_devices", return_value=[{"id": "selected"}]), \
             mock.patch.object(backend.subprocess, "Popen", return_value=process), \
             mock.patch.object(backend, "PROTOCOL_OUTPUT", output):
            with self.assertRaisesRegex(ValueError, "Free signing limit reached"):
                backend.deploy(Path("/runtime"), Path("/workspace"), "selected", {})
        self.assertEqual(json.loads(output.getvalue())["message"], message)

    def test_profile_team_metadata_is_strictly_validated(self) -> None:
        valid = {"Platform": ["iOS"], "TeamIdentifier": ["ABCDEFGHIJ"], "TeamName": "Example"}
        self.assertEqual(backend.profile_team(valid), {"id": "ABCDEFGHIJ", "name": "Example"})
        for value in (None, {}, {**valid, "Platform": None}, {**valid, "TeamIdentifier": "bad"},
                      {**valid, "TeamName": ""}, {**valid, "Platform": ["macOS"]}):
            with self.subTest(value=value):
                self.assertIsNone(backend.profile_team(value))

    def test_invalid_zip_returns_protocol_error_without_traceback(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            archive = directory / "invalid.zip"
            archive.write_bytes(b"not a ZIP")
            result = subprocess.run(
                [sys.executable, "-B", str(TOOLS / "installer_backend.py"),
                 "--source", str(TOOLS.parent), "--workspace", str(directory / "workspace"),
                 "import", str(archive)], text=True, capture_output=True,
            )
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, "")
        messages = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(messages[-1]["type"], "error")

    def test_ui_contract_labels_and_section_order(self) -> None:
        root = TOOLS.parent
        contract = json.loads((root / "config/installer_ui.json").read_text())
        self.assertEqual([section["id"] for section in contract["sections"]],
                         ["release", "device", "signing"])
        import re
        used = set()
        for path in (root / "installer").glob("*.swift"):
            used.update(re.findall(r'(?:text|layout\.text)\("([A-Za-z]+)"\)', path.read_text()))
        self.assertFalse(used - set(contract["labels"]))
        info = plistlib.loads((root / "installer/Info.plist").read_bytes())
        self.assertEqual(info["LSMinimumSystemVersion"], "26.6")
