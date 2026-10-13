#!/usr/bin/env python3
"""Create an ad-hoc signed private Mac Builder with offline native IPA support."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

from native_macos_package import validate_native_kit
from mac_portable_runtime import install_runtime, ARCHIVE_NAME
from native_macho import file_sha256
from prepare_native_aot import compile_host_compiler

sys.dont_write_bytecode = True


def build_private_app(source: Path, template: Path, catalog: Path,
                      output: Path, python_archive: Path) -> None:
    if source.is_symlink() or not source.is_dir() or source.suffix != ".app":
        raise ValueError("Mac installer source must be an existing .app")
    if not template.is_file() or template.is_symlink() or template.suffix != ".zip":
        raise ValueError("the unsigned template is missing")
    if not python_archive.is_file() or python_archive.is_symlink():
        raise ValueError("verified standalone Python archive is required")
    if not catalog.is_file() or catalog.is_symlink() or catalog.name != "hu.xlf":
        raise ValueError("private Hungarian hu.xlf is missing")
    if output.exists() or output.is_symlink() or output.suffix != ".app":
        raise ValueError("private output must be a new .app")
    if output.parent.is_symlink() or not output.parent.is_dir():
        raise ValueError("private output parent is invalid")
    try:
        shutil.copytree(source, output, symlinks=False)
        kit = output / "Contents/Resources/NativeKit"
        kit.mkdir(mode=0o700)
        shutil.copy2(template, kit / "template.zip")
        shutil.copy2(catalog, kit / "hu.xlf")
        compile_host_compiler(kit / "a64-compiler")
        config = {
            "version": 1,
            "template_sha256": file_sha256(kit / "template.zip"),
            "hungarian_sha256": file_sha256(kit / "hu.xlf"),
            "compiler_sha256": file_sha256(kit / "a64-compiler"),
        }
        (kit / "manifest.json").write_text(
            json.dumps(config, sort_keys=True, indent=2) + "\n", encoding="utf-8"
        )
        validate_native_kit(kit)
        install_runtime(python_archive, output / "Contents/Resources/PythonRuntime")
        subprocess.run(["codesign", "--force", "--deep", "--sign", "-",
                        "--options", "runtime", str(output)],
                       check=True, timeout=120)
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(output)],
                       check=True, timeout=60)
    except BaseException:
        shutil.rmtree(output)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--hungarian-xlf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--python-runtime-archive", type=Path,
                        default=Path(__file__).resolve().parents[1] /
                                "build/private-runtime-cache" / ARCHIVE_NAME,
                        help="checksum-pinned offline macOS aarch64 standalone Python archive")
    args = parser.parse_args()
    try:
        build_private_app(args.app, args.template, args.hungarian_xlf, args.output,
                          args.python_runtime_archive)
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        print("Mac private Builder packaging failed:", exc, file=sys.stderr)
        return 1
    print("Private Mac Builder:", args.output)
    print("It requires a legally obtained original ZIP to generate an IPA.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
