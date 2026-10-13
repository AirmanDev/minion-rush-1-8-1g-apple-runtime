#!/usr/bin/env python3
"""Unpack a pinned, relocatable Apple Silicon Python for a portable Mac app."""

from __future__ import annotations

import shutil
import subprocess
import tarfile
from pathlib import Path, PurePosixPath

from native_macho import file_sha256

PYTHON_VERSION = "3.14.8"
ARCHIVE_SHA256 = "ed14e9042f36c0aa18383f5d9729bcbf17847e842666a793b5a5a5d8505c0ca1"
ARCHIVE_NAME = "cpython-3.14.8-aarch64-macos.tar.gz"
ORIGIN = ("astral-sh/python-build-standalone release 20261003, "
          "cpython-3.14.8+20261003-aarch64-apple-darwin-install_only_stripped.tar.gz")
MAGIC_ARM64 = b"\xcf\xfa\xed\xfe"
# Non-runtime developer tools and Tcl/Tk are not used by the SwiftUI Builder.
# The pinned archive remains untouched for reproducibility and licenses.
SKIP_ROOTS = frozenset({
    "tcl9.0", "tcl9", "tk9.0", "itcl4.3.8", "thread3.0.6", "pkgconfig",
})
SKIP_LIBRARIES = frozenset({
    "libtcl9.0.dylib", "libtcl9tk9.0.dylib",
})
SKIP_STDLIB = frozenset({
    "idlelib", "ensurepip", "tkinter", "site-packages",
    "turtledemo", "pydoc_data",
})


def install_runtime(archive: Path, destination: Path) -> None:
    if file_sha256(archive) != ARCHIVE_SHA256:
        raise ValueError("portable Python archive checksum does not match pinned upstream release")
    destination = destination.absolute()
    if destination.exists() or destination.is_symlink():
        raise ValueError("Python runtime destination already exists")
    destination.mkdir(mode=0o755)
    files = 0
    try:
        with tarfile.open(archive, "r:gz") as source:
            for member in source:
                path = PurePosixPath(member.name)
                if not path.parts or path.parts[0] != "python":
                    continue
                parts = path.parts[1:]
                if not parts:
                    continue
                if not (parts == ("bin", "python3.14") or parts[0] == "lib"):
                    continue
                if parts[0] == "lib":
                    if len(parts) > 1 and (parts[1] in SKIP_ROOTS or
                                           parts[1] in SKIP_LIBRARIES):
                        continue
                    if len(parts) > 2 and parts[1] == "python3.14":
                        if parts[2] in SKIP_STDLIB:
                            continue
                        if (parts[2] == "lib-dynload" and len(parts) > 3 and
                                parts[3].startswith("_tkinter.")):
                            continue
                if any(part in ("", ".", "..") for part in parts):
                    raise ValueError("unsafe portable Python archive path")
                # Metadata pkgconfig symlinks are for development, not the app.
                if member.issym() and parts[:2] == ("lib", "pkgconfig"):
                    continue
                if not member.isfile() and not member.isdir():
                    raise ValueError("unexpected archive entry in bundled Python")
                target = destination.joinpath(*parts)
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                file = source.extractfile(member)
                if file is None or member.size > 64 * 1024 * 1024:
                    raise ValueError("unexpected Python archive member size")
                with file, target.open("xb") as output:
                    shutil.copyfileobj(file, output, 1024 * 1024)
                if target.stat().st_size != member.size:
                    raise ValueError("incomplete Python runtime extraction")
                target.chmod(0o755 if target.name == "python3.14" or
                             target.suffix in (".dylib", ".so") else 0o644)
                files += 1
        runtime = destination / "bin/python3.14"
        license_file = destination / "lib/python3.14/LICENSE.txt"
        if files < 400 or not runtime.is_file() or not license_file.is_file():
            raise ValueError("portable Python runtime is incomplete")
        def is_macho(path: Path) -> bool:
            if not path.is_file(): return False
            with path.open("rb") as stream:
                return stream.read(4) == MAGIC_ARM64
        embedded_macho = [item for item in destination.rglob("*") if is_macho(item)]
        if runtime not in embedded_macho:
            raise ValueError("embedded Python is not an arm64 macOS executable")
        for binary in embedded_macho:
            subprocess.run(["codesign", "--force", "--sign", "-", str(binary)],
                           check=True, timeout=30)
        result = subprocess.run(
            [str(runtime), "-I", "-B", "-c",
             "import sys,ctypes,hashlib,sqlite3,ssl,json,zipfile;"
             "assert sys.prefix == sys.base_prefix;"
             "assert sys.prefix == sys.argv[1];"
             "print(sys.version.split()[0])", str(destination)],
            capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            raise ValueError("embedded Python self-test failed: " + result.stderr[-1600:])
        if result.stdout.strip() != PYTHON_VERSION:
            raise ValueError("unexpected portable Python version")
        (destination / "ORIGIN.txt").write_text(
            f"{ORIGIN}\nSHA256: {ARCHIVE_SHA256}\n"
            "This package includes Python and third-party components; see "
            "lib/python3.14/LICENSE.txt and bundled upstream licenses.\n",
            encoding="ascii")
    except BaseException:
        shutil.rmtree(destination)
        raise
