#!/usr/bin/env python3
"""UTM-only local SPICE/WebDAV ZIP import for private Windows test kits."""

from __future__ import annotations

import os
import re
import urllib.parse
import urllib.request
from pathlib import Path, PureWindowsPath

from install_assets import MAX_ARCHIVE_BYTES

def _mapped_unc_root(letter: str) -> str | None:
    """Return the actual mapped Windows network resource, never guess from Z:."""
    if os.name != "nt" or not re.fullmatch(r"[A-Za-z]:", letter):
        return None
    import ctypes
    from ctypes import wintypes

    buffer = ctypes.create_unicode_buffer(32768)
    capacity = wintypes.DWORD(len(buffer))
    result = ctypes.windll.mpr.WNetGetConnectionW(
        letter, buffer, ctypes.byref(capacity)
    )
    return buffer.value if result == 0 else None


def _utm_webdav_url(source: Path) -> str | None:
    """Restrict direct HTTP access to the UTM SPICE server on guest loopback.

    Windows WebClient's mapped drive may refuse to open ZIPs larger than its
    configured file-size limit. The separate UTM SPICE WebDAV service handles
    ordinary HTTP GET without invoking that Windows filesystem redirector.
    """
    if os.name != "nt":
        return None
    path = PureWindowsPath(str(source))
    if path.drive.startswith("\\\\"):
        remote = path.drive
    else:
        remote = _mapped_unc_root(path.drive)
    if not remote or remote.lower().rstrip("\\") != r"\\localhost@9843\davwwwroot":
        return None
    parts = path.parts[1:]
    if (not parts or any(part in ("", ".", "..") for part in parts)
            or any("\x00" in part or "/" in part or "\\" in part for part in parts)):
        raise ValueError("unsafe UTM shared ZIP path")
    encoded = "/".join(urllib.parse.quote(part, safe="") for part in parts)
    return "http://127.0.0.1:9843/" + encoded


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        return None


def _utm_http_stream(url: str):
    """No proxies, cookies, credentials, redirects, or non-loopback hosts."""
    if not url.startswith("http://127.0.0.1:9843/"):
        raise ValueError("unsafe UTM share URL")
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({}), _NoRedirect()
    )
    response = opener.open(
        urllib.request.Request(url, method="GET"), timeout=60
    )
    length = response.headers.get("Content-Length")
    if length is not None:
        try:
            size = int(length)
        except ValueError as exc:
            response.close()
            raise ValueError("invalid UTM HTTP ZIP length") from exc
        if size <= 0 or size > MAX_ARCHIVE_BYTES:
            response.close()
            raise ValueError("UTM HTTP ZIP exceeds supported size")
    return response
