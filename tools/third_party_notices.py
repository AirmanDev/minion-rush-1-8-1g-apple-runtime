#!/usr/bin/env python3
"""Source-verified third-party Python notices for portable builders."""

from __future__ import annotations

import hashlib
from pathlib import Path

COMMON_NOTICES = {
    'python-expat-LICENSE.txt': '122f2c27000472a201d337b9b31f7eb2b52d091b02857061a8880371612d9534',
    'python-liblzma-LICENSE.txt': '9a4062de0a2c388a98cf35a35d348b62fa97c838a71c3c28ee1a2d7d0a565b02',
    'python-libtommath-LICENSE.txt': '2fa64b163659f41965c9815882a8296d3d03ff546b76153e11445f9bdecf955a',
    'python-mpdecimal-LICENSE.txt': '669512af7219f58be03a398766d7c9da11a3b3df9d3f05cb74c5ceca25c8da3b',
    'python-openssl-3-LICENSE.txt': '7d5450cb2d142651b8afa315b5f238efc805dad827d91ba367d8516bc9d49e7a',
    'python-zstd-LICENSE.txt': '7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8',
}

MAC_ONLY_NOTICES = {
    'bzip2-LICENSE.txt': '1f38bbc7caacafd65169276d759c0d88c991b753b643ce35d0e45ea1971dd441',
    'libffi-LICENSE.txt': 'deaf3a42effb551a5b140fa9afefed183a27f1341c6d1bf430d106a5e6931fc0',
}

TOOLCHAIN_NOTICES = {
    'gcc-COPYING.RUNTIME.txt': '9d6b43ce4d8de0c878bf16b54d8e7a10d9bd42b75178153e3af6a815bdc90f74',
    'gcc-COPYING3.txt': '8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903',
}

def verify_notice(path: Path, digest: str) -> None:
    if path.is_symlink() or not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise ValueError("missing or modified third-party license notice: " + str(path))
