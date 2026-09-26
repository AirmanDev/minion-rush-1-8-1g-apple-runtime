#!/usr/bin/env python3
"""JSON Lines bridge between native installer clients and the existing tools."""

from __future__ import annotations

import argparse
import contextlib
import fcntl
import hashlib
import io
import json
import os
import platform
import plistlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import uuid
import zipfile
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterator

sys.dont_write_bytecode = True

from install_assets import install_release
from list_ios_devices import paired_physical_devices
from signing import signing_settings
from validate_assets import inspect_assets

PROTOCOL = 1
PROTOCOL_OUTPUT = sys.stdout
OPERATION_LOG: io.TextIOBase | None = None


def emit(kind: str, **fields: object) -> None:
    if OPERATION_LOG is not None:
        message = fields.get("message")
        if isinstance(message, str):
            OPERATION_LOG.write((message if kind == "log" else f"[{kind}] {message}") + "\n")
        elif kind == "result":
            OPERATION_LOG.write("Operation completed successfully.\n")
    print(json.dumps({"protocol": PROTOCOL, "type": kind, **fields}),
          file=PROTOCOL_OUTPUT, flush=True)


@contextlib.contextmanager
def operation_log(workspace: Path) -> Iterator[None]:
    global OPERATION_LOG
    if workspace.is_symlink():
        raise ValueError("The installer workspace must not be a symbolic link.")
    directory = workspace / "logs"
    if directory.is_symlink():
        raise ValueError("The log directory must not be a symbolic link.")
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    path = directory / f"{timestamp}-{uuid.uuid4().hex}.log"
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    previous = OPERATION_LOG
    with os.fdopen(descriptor, "w", encoding="utf-8", buffering=1) as output:
        OPERATION_LOG = output
        try:
            emit("session", log_path=str(path))
            yield
        finally:
            OPERATION_LOG = previous


class LogStream(io.TextIOBase):
    def write(self, value: str) -> int:
        for line in value.splitlines():
            if line:
                emit("log", message=line)
        return len(value)


@contextlib.contextmanager
def workspace_lock(workspace: Path) -> Iterator[None]:
    if workspace.is_symlink():
        raise ValueError("The installer workspace must not be a symbolic link.")
    workspace.mkdir(parents=True, exist_ok=True)
    with (workspace / ".lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise ValueError("Another installer operation is running.") from exc
        yield


def public_files(source: Path) -> list[Path]:
    names = (source / "config/source_manifest.txt").read_text().splitlines()
    files: list[Path] = []
    for name in names:
        relative = Path(name)
        if not name or relative.is_absolute() or ".." in relative.parts:
            raise ValueError("Invalid public source manifest.")
        path = source / relative
        if path.is_symlink() or not path.is_file() or source not in path.resolve().parents:
            raise ValueError(f"Missing or unsafe public source file: {name}")
        files.append(path)
    return files


def prepare_runtime(source: Path, workspace: Path) -> Path:
    files = public_files(source)
    digest = hashlib.sha256()
    for path in files:
        digest.update(path.relative_to(source).as_posix().encode() + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    versions = workspace / "runtimes"
    if versions.is_symlink():
        raise ValueError("The runtimes directory must not be a symbolic link.")
    versions.mkdir(exist_ok=True)
    runtime = versions / digest.hexdigest()
    if runtime.is_symlink():
        raise ValueError("The runtime directory must not be a symbolic link.")
    if not runtime.exists():
        with tempfile.TemporaryDirectory(prefix=".source-", dir=versions) as temporary:
            staged = Path(temporary) / "runtime"
            staged.mkdir()
            for path in files:
                destination = staged / path.relative_to(source)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, destination)
            staged.rename(runtime)
    return runtime


def command_environment(developer: str) -> dict[str, str]:
    environment = dict(os.environ)
    for key in list(environment):
        if key.startswith("MR_") or key in ("CC", "PYTHON", "PYTHONPATH", "BASH_ENV"):
            del environment[key]
    environment.update(
        DEVELOPER_DIR=developer, PYTHONDONTWRITEBYTECODE="1", PYTHON=sys.executable,
        PATH=f"{Path(sys.executable).parent}:/usr/bin:/bin:/usr/sbin:/sbin",
    )
    return environment


def run_command(
    arguments: list[str], environment: dict[str, str], cwd: Path | None = None,
    timeout: int = 30,
) -> str:
    result = subprocess.run(
        arguments, cwd=cwd, env=environment, text=True, capture_output=True,
        check=False, timeout=timeout,
    )
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()[-2000:]
        raise ValueError(f"{Path(arguments[0]).name} failed: {detail}")
    return result.stdout


def toolchain() -> dict[str, str]:
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise ValueError("Device installation requires an Apple Silicon Mac.")
    developer = os.environ.get("DEVELOPER_DIR", "")
    if not developer:
        developer = run_command(["/usr/bin/xcode-select", "-p"], dict(os.environ)).strip()
        if developer.endswith("/CommandLineTools"):
            developer = "/Applications/Xcode.app/Contents/Developer"
    if not (Path(developer) / "Platforms/iPhoneOS.platform").is_dir():
        raise ValueError("Install Xcode 27 with iOS platform support and open it once.")
    environment = command_environment(developer)
    version = run_command(["/usr/bin/xcrun", "xcodebuild", "-version"], environment)
    match = re.search(r"Xcode (\d+)", version)
    if not match or int(match[1]) < 27:
        raise ValueError("Xcode 27 or later is required to build this runtime.")
    run_command(["/usr/bin/xcrun", "--sdk", "iphoneos", "--show-sdk-path"], environment)
    return {"developer": developer, "version": version.strip()}


def query_devices(environment: dict[str, str]) -> list[dict[str, str]]:
    with tempfile.TemporaryDirectory(prefix="minion-rush-devices.") as temporary:
        output = Path(temporary) / "devices.json"
        run_command(
            ["/usr/bin/xcrun", "devicectl", "list", "devices",
             "--omit-deprecated-fields-in-json", "--json-output", str(output)],
            environment,
        )
        payload = json.loads(output.read_text())
    return paired_physical_devices(payload, [])


def profile_team(profile: object) -> dict[str, str] | None:
    if not isinstance(profile, dict):
        return None
    platforms = profile.get("Platform")
    if not isinstance(platforms, list) or "iOS" not in platforms:
        return None
    identifiers = profile.get("TeamIdentifier", [])
    name = profile.get("TeamName")
    if (not isinstance(identifiers, list) or len(identifiers) != 1
            or not isinstance(identifiers[0], str)
            or not re.fullmatch(r"[A-Z0-9]{10}", identifiers[0])
            or not isinstance(name, str) or not name):
        return None
    return {"id": identifiers[0], "name": name}


def discover_teams(environment: dict[str, str]) -> list[dict[str, str]]:
    directory = Path.home() / "Library/Developer/Xcode/UserData/Provisioning Profiles"
    teams: dict[str, dict[str, str]] = {}
    for path in sorted(directory.glob("*.mobileprovision"))[:100]:
        try:
            if path.is_symlink() or path.stat().st_size > 4 * 1024 * 1024:
                continue
            decoded = run_command(["/usr/bin/security", "cms", "-D", "-i", str(path)],
                                  environment, timeout=3)
            team = profile_team(plistlib.loads(decoded.encode()))
            if team:
                teams[team["id"]] = team
        except (OSError, ValueError, plistlib.InvalidFileException, subprocess.SubprocessError):
            continue
    return sorted(teams.values(), key=lambda team: (team["name"].casefold(), team["id"]))


def deploy(runtime: Path, workspace: Path, device: str, environment: dict[str, str]) -> None:
    devices = query_devices(environment)
    if device not in {item["id"] for item in devices}:
        raise ValueError("The selected device is no longer paired or available. Refresh devices.")
    environment.update(
        MR_ASSETS_ROOT=str(workspace / "assets"),
        MR_IOS_DERIVED=str(workspace / "DerivedData"),
    )
    process = subprocess.Popen(
        ["/bin/bash", str(runtime / "tools/deploy_ios.sh"), "--identifier", device],
        cwd=runtime, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL, text=True, errors="replace", start_new_session=True,
    )
    completed = False
    try:
        assert process.stdout is not None
        failure = "Installation failed. See the activity log."
        for line in process.stdout:
            line = line.rstrip()
            emit("log", message=line)
            if "maximum number of installed apps using a free developer profile" in line.casefold():
                failure = "Free signing limit reached (3 apps). See the activity log."
            if line.startswith("== ") and line.endswith(" =="):
                emit("progress", message=line[3:-3])
        status = process.wait()
        completed = True
        if status != 0:
            raise ValueError(failure)
    finally:
        if not completed:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        if process.stdout:
            process.stdout.close()


def execute(args: argparse.Namespace) -> dict[str, object]:
    source = args.source.resolve()
    workspace = args.workspace.resolve()
    with workspace_lock(args.workspace):
        emit("progress", message="Preparing runtime")
        runtime = prepare_runtime(source, workspace)
        assets = workspace / "assets"
        if args.command == "import":
            if args.archive.suffix.casefold() != ".zip" or not args.archive.is_file():
                raise ValueError("Choose one original 1.8.1g release ZIP.")
            emit("progress", message="Importing and validating release assets")
            with contextlib.redirect_stdout(LogStream()):
                report = install_release(args.archive, runtime, assets)
            return {"assets": asdict(report), "runtime": str(runtime)}
        if args.command == "status":
            result: dict[str, object] = {"runtime": str(runtime)}
            try:
                result["assets"] = asdict(inspect_assets(runtime, assets))
            except (OSError, ValueError) as exc:
                result["asset_error"] = str(exc)
            try:
                chain = toolchain()
                result["toolchain"] = chain["version"]
                environment = command_environment(chain["developer"])
                result["devices"] = query_devices(environment)
                result["teams"] = discover_teams(environment)
            except (OSError, ValueError, subprocess.SubprocessError) as exc:
                result["toolchain_error"] = str(exc)
                result["devices"] = []
            return result
        chain = toolchain()
        emit("log", message=chain["version"])
        environment = command_environment(chain["developer"])
        if args.command == "devices":
            return {"devices": query_devices(environment)}
        environment.update(signing_settings(args.team, args.bundle))
        emit("progress", message="Validating imported assets")
        report = inspect_assets(runtime, assets)
        emit("log", message=report.summary())
        emit("log", message=f"Installing bundle: {args.bundle}")
        deploy(runtime, workspace, args.device, environment)
        return {"installed": True, "device": args.device}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    commands.add_parser("devices")
    importer = commands.add_parser("import")
    importer.add_argument("archive", type=Path)
    installer = commands.add_parser("install")
    installer.add_argument("--device", required=True)
    installer.add_argument("--team", required=True)
    installer.add_argument("--bundle", required=True)
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, cancel_operation)
    with contextlib.ExitStack() as resources:
        try:
            resources.enter_context(operation_log(args.workspace))
            emit("log", message=f"Operation: {args.command}")
            result = execute(args)
        except (KeyboardInterrupt, InterruptedError):
            emit("cancelled", message="Operation cancelled.")
            return 130
        except (OSError, ValueError, RuntimeError, zipfile.BadZipFile,
                subprocess.SubprocessError) as exc:
            emit("error", message=str(exc))
            return 1
        emit("result", **result)
        return 0


def cancel_operation(signum: int, frame: object) -> None:
    raise InterruptedError("Operation cancelled.")


if __name__ == "__main__":
    raise SystemExit(main())
