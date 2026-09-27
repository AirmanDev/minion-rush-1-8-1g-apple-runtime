"""Install an already signed runtime IPA using Apple's USB device service."""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import logging
import os
import plistlib
import sys
import threading
import uuid
from dataclasses import asdict
from pathlib import Path

from installer_protocol import emit, operation_log, workspace_lock
from ipa_archive import IPAReport, inspect_ipa


def decode_profile(data: bytes) -> dict:
    from asn1crypto.cms import ContentInfo
    message = ContentInfo.load(data, strict=True)
    if message["content_type"].native != "signed_data":
        raise ValueError("The provisioning profile is not CMS signed data.")
    content = message["content"]["encap_content_info"]["content"].native
    if not isinstance(content, bytes):
        raise ValueError("The provisioning profile has no plist content.")
    return plistlib.loads(content)


def os_version(value: str) -> tuple[int, ...]:
    try:
        parts = [int(part) for part in value.split(".")]
        if not 1 <= len(parts) <= 3 or any(part < 0 for part in parts):
            raise ValueError
        return tuple(parts + [0] * (3 - len(parts)))
    except (ValueError, AttributeError) as exc:
        raise ValueError("Invalid device operating system version.") from exc


def check_device(report: IPAReport, device: dict[str, str]) -> None:
    if device["udid"] not in report.devices:
        raise ValueError("This device is not in the IPA's profile. Export a new IPA for it on the Mac.")
    if os_version(device["os"]) < max(os_version("17"), os_version(report.minimum_os)):
        raise ValueError("This device's operating system is older than the IPA requires.")


async def query_devices() -> list[dict[str, str]]:
    from pymobiledevice3 import usbmux
    from pymobiledevice3.lockdown import create_using_usbmux
    result = []
    for device in await usbmux.list_devices():
        if not device.is_usb:
            continue
        try:
            async with await create_using_usbmux(
                device.serial, autopair=False, connection_type="USB"
            ) as client:
                if not await client.validate_pairing():
                    emit("log", message="A device is not paired. Unlock it and trust this computer in iTunes.")
                    continue
                info = client.short_info
                if (info.get("DeviceClass") not in ("iPhone", "iPad")
                        or os_version(info.get("ProductVersion", "0")) < os_version("17")):
                    continue
                result.append({"id": device.serial, "udid": client.udid,
                               "name": info.get("DeviceName") or "iPhone or iPad",
                               "os": info["ProductVersion"]})
        except (OSError, ValueError) as exc:
            emit("log", message=f"Device unavailable: {exc}")
    return sorted(result, key=lambda item: (item["name"].casefold(), item["id"]))


async def upload_ipa(afc: object, archive: Path, remote: str) -> None:
    size = archive.stat().st_size
    handle = await afc.fopen(remote, "w")
    try:
        sent = 0
        previous = -1
        with archive.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                await afc.fwrite(handle, chunk)
                sent += len(chunk)
                percent = sent * 100 // max(size, 1)
                if percent != previous:
                    emit("progress", message=f"Uploading IPA: {percent}%")
                    previous = percent
    finally:
        await afc.fclose(handle)


async def install(archive: Path, device_id: str, report: IPAReport) -> None:
    from pymobiledevice3.lockdown import create_using_usbmux
    from pymobiledevice3.services.afc import AfcService
    from pymobiledevice3.services.installation_proxy import InstallationProxyService
    devices = await query_devices()
    device = next((item for item in devices if item["id"] == device_id), None)
    if device is None:
        raise ValueError("The selected USB device is no longer available. Refresh devices.")
    check_device(report, device)
    async with await create_using_usbmux(
        device_id, autopair=False, connection_type="USB"
    ) as client:
        if not await client.validate_pairing():
            raise ValueError("Device pairing is no longer valid. Unlock it and trust this computer.")
        async with AfcService(client) as afc, InstallationProxyService(client) as proxy:
            staging = "/PublicStaging/MinionRushInstaller"
            remote = staging + "/" + uuid.uuid4().hex + ".ipa"
            await afc.makedirs(staging)
            try:
                await upload_ipa(afc, archive, remote)
                emit("progress", message="Installing signed application")
                await proxy.send_package(
                    "Install", {},
                    lambda percent, *args: emit("progress", message=f"Installing: {percent}%"),
                    remote,
                )
                emit("progress", message="Verifying installed application")
                apps = await proxy.get_apps(bundle_identifiers=[report.bundle])
                if report.bundle not in apps:
                    raise ValueError("The device did not report the application after installation.")
            finally:
                try:
                    await asyncio.wait_for(afc.rm_single(remote, force=True), timeout=5)
                except (Exception, asyncio.CancelledError):
                    emit("log", message="The device staging file could not be removed after disconnect or cancellation.")


async def execute(args: argparse.Namespace) -> dict:
    if args.command == "check":
        from importlib.metadata import version
        from asn1crypto.cms import ContentInfo
        from pymobiledevice3.lockdown import create_using_usbmux
        from pymobiledevice3.services.afc import AfcService
        from pymobiledevice3.services.installation_proxy import InstallationProxyService
        if not all(callable(value) for value in (ContentInfo.load, create_using_usbmux,
                                                AfcService.fwrite, InstallationProxyService.send_package)):
            raise ValueError("The packaged device API is unavailable.")
        return {"device_backend": version("pymobiledevice3")}
    if args.command == "devices":
        emit("progress", message="Checking connected USB devices")
        return {"devices": await query_devices()}
    emit("progress", message="Inspecting signed IPA")
    contract = Path(sys.executable).parent.parent / "installer_ui.json" if getattr(sys, "frozen", False) else None
    report = inspect_ipa(args.archive, decode_profile, rules_path=contract)
    emit("log", message=f"Bundle: {report.bundle}; team: {report.team}; expires: {report.expires}")
    if args.command == "inspect":
        return {"ipa": asdict(report)}
    await install(args.archive, args.device, report)
    emit("log", message="Installation verified. Open the game on the device to check startup.")
    return {"installed": True}


class ProtocolLogHandler(logging.Handler):
    def emit(self, record: logging.LogRecord) -> None:
        emit("log", message=self.format(record))


async def run(args: argparse.Namespace) -> int:
    task = asyncio.create_task(execute(args))
    loop = asyncio.get_running_loop()

    def read_cancel() -> None:
        try:
            pending = bytearray()
            while len(pending) < 128:
                chunk = os.read(sys.stdin.fileno(), 128 - len(pending))
                if not chunk:
                    loop.call_soon_threadsafe(task.cancel)
                    return
                pending.extend(chunk)
                if b"\n" in pending:
                    if pending.strip() == b"cancel":
                        loop.call_soon_threadsafe(task.cancel)
                    return
        except (OSError, RuntimeError):
            pass

    threading.Thread(target=read_cancel, daemon=True).start()
    try:
        result = await task
        emit("result", **result)
        return 0
    except asyncio.CancelledError:
        emit("cancelled", message="Operation cancelled. A started device installation may still finish.")
        return 130


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("devices")
    commands.add_parser("check")
    for command in ("inspect", "install"):
        operation = commands.add_parser(command)
        operation.add_argument("archive", type=Path)
        if command == "install":
            operation.add_argument("--device", required=True)
    args = parser.parse_args()
    logging.basicConfig(level=logging.WARNING, handlers=[ProtocolLogHandler()])
    with contextlib.ExitStack() as resources:
        try:
            resources.enter_context(operation_log(args.workspace))
            resources.enter_context(workspace_lock(args.workspace))
            emit("log", message=f"Operation: {args.command}")
            return asyncio.run(run(args))
        except Exception as exc:
            emit("error", message=f"{type(exc).__name__}: {exc}. For USB errors, check iTunes, trust, and the Apple Mobile Device Service.")
            return 1


if __name__ == "__main__":
    raise SystemExit(main())
