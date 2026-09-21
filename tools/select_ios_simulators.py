#!/usr/bin/env python3
"""Select the production iOS runtime and representative simulator devices."""

from __future__ import annotations

import json
import sys
from pathlib import Path


PHONE_PREFERENCES = (
    "iPhone SE (2nd generation)",
    "iPhone SE (3rd generation)",
    "iPhone 11",
)
TABLET_PREFERENCES = (
    "iPad (10th generation)",
    "iPad (A16)",
    "iPad mini (6th generation)",
)


def available_ios_26_runtime(payload: dict[str, object]) -> str:
    candidates: list[tuple[tuple[int, ...], str]] = []
    for runtime in payload.get("runtimes", []):
        if not isinstance(runtime, dict) or not runtime.get("isAvailable", False):
            continue
        identifier = runtime.get("identifier")
        version = runtime.get("version")
        if not isinstance(identifier, str) or not isinstance(version, str):
            continue
        parts = version.split(".")
        if not parts or parts[0] != "26" or not all(part.isdigit() for part in parts):
            continue
        candidates.append((tuple(int(part) for part in parts), identifier))
    if not candidates:
        raise ValueError("an available iOS 26 Simulator runtime is required")
    return max(candidates)[1]


def preferred_device_type(payload: dict[str, object], names: tuple[str, ...]) -> str:
    devices = {
        device.get("name"): device.get("identifier")
        for device in payload.get("devicetypes", [])
        if isinstance(device, dict)
        and isinstance(device.get("name"), str)
        and isinstance(device.get("identifier"), str)
    }
    for name in names:
        if name in devices:
            return devices[name]
    raise ValueError("required Simulator device type is unavailable: " + ", ".join(names))


def select(runtime_payload: dict[str, object], device_payload: dict[str, object]) -> tuple[str, str, str]:
    return (
        available_ios_26_runtime(runtime_payload),
        preferred_device_type(device_payload, PHONE_PREFERENCES),
        preferred_device_type(device_payload, TABLET_PREFERENCES),
    )


def main() -> int:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <runtimes.json> <device-types.json>", file=sys.stderr)
        return 2
    try:
        runtime_payload = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
        device_payload = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
        print("\t".join(select(runtime_payload, device_payload)))
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
