from __future__ import annotations

import asyncio
import io
import json
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
sys.dont_write_bytecode = True

import installer_protocol as protocol
import ipa_device_backend as backend
from ipa_archive import IPAReport


class DeviceBackendTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.archive = Path(temporary.name) / "game.ipa"
        self.archive.write_bytes(b"x" * (3 * 1024**2 + 10))
        self.report = IPAReport("org.example.game", "ABCDEFGHIJ",
                                (datetime.now(timezone.utc) + timedelta(days=1)).isoformat(),
                                ["registered"], "17.0", self.archive.stat().st_size)
        self.device = {"id": "selected", "udid": "registered", "os": "27.0"}

    async def test_upload_is_chunked_and_closes_its_handle(self) -> None:
        afc = mock.AsyncMock()
        afc.fopen.return_value = 42
        output = io.StringIO()
        with mock.patch.object(protocol, "PROTOCOL_OUTPUT", output):
            await backend.upload_ipa(afc, self.archive, "/own-stage.ipa")
        chunks = [call.args[1] for call in afc.fwrite.call_args_list]
        self.assertEqual(max(map(len, chunks)), 1024**2)
        self.assertEqual(b"".join(chunks), self.archive.read_bytes())
        afc.fclose.assert_awaited_once_with(42)
        self.assertIn("100%", output.getvalue())

    async def test_cancelled_upload_closes_handle(self) -> None:
        afc = mock.AsyncMock()
        afc.fopen.return_value = 42
        afc.fwrite.side_effect = asyncio.CancelledError
        with self.assertRaises(asyncio.CancelledError):
            await backend.upload_ipa(afc, self.archive, "/own-stage.ipa")
        afc.fclose.assert_awaited_once_with(42)

    async def test_device_mismatch_never_starts_an_install(self) -> None:
        modules = {name: SimpleNamespace(**values) for name, values in {
            "pymobiledevice3.lockdown": {"create_using_usbmux": mock.AsyncMock()},
            "pymobiledevice3.services.afc": {"AfcService": mock.Mock()},
            "pymobiledevice3.services.installation_proxy": {"InstallationProxyService": mock.Mock()},
        }.items()}
        with mock.patch.dict(sys.modules, modules), mock.patch.object(backend, "query_devices", new=mock.AsyncMock(return_value=[])):
            with self.assertRaisesRegex(ValueError, "no longer"):
                await backend.install(self.archive, "selected", self.report)
        modules["pymobiledevice3.lockdown"].create_using_usbmux.assert_not_awaited()

    async def test_install_and_failure_cleanup_only_own_staging_file(self) -> None:
        for failure in (None, ValueError("Device rejected signature")):
            with self.subTest(failure=failure):
                client = mock.AsyncMock()
                client.__aenter__.return_value = client
                client.validate_pairing.return_value = True
                afc = mock.AsyncMock()
                afc.__aenter__.return_value = afc
                proxy = mock.AsyncMock()
                proxy.__aenter__.return_value = proxy
                proxy.send_package.side_effect = failure
                proxy.get_apps.return_value = {self.report.bundle: {}}
                modules = {
                    "pymobiledevice3.lockdown": SimpleNamespace(create_using_usbmux=mock.AsyncMock(return_value=client)),
                    "pymobiledevice3.services.afc": SimpleNamespace(AfcService=mock.Mock(return_value=afc)),
                    "pymobiledevice3.services.installation_proxy": SimpleNamespace(InstallationProxyService=mock.Mock(return_value=proxy))}
                with mock.patch.dict(sys.modules, modules), \
                     mock.patch.object(backend, "query_devices", new=mock.AsyncMock(return_value=[self.device])), \
                     mock.patch.object(protocol, "PROTOCOL_OUTPUT", io.StringIO()):
                    if failure:
                        with self.assertRaisesRegex(ValueError, "signature"):
                            await backend.install(self.archive, "selected", self.report)
                    else:
                        await backend.install(self.archive, "selected", self.report)
                remote = proxy.send_package.call_args.args[3]
                self.assertTrue(remote.startswith("/PublicStaging/MinionRushInstaller/"))
                afc.rm_single.assert_awaited_once_with(remote, force=True)
                proxy.uninstall.assert_not_awaited()


if __name__ == "__main__":
    unittest.main()
