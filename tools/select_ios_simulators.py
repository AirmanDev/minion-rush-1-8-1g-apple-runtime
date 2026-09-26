#!/usr/bin/env python3
"""Select one installed runtime per supported iOS family and matching devices."""

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


def available_runtimes(payload: dict[str, object], minimum: str,
                       requested: set[int]) -> list[dict[str, object]]:
    minimum_parts = tuple(int(part) for part in minimum.split("."))
    candidates: dict[int, tuple[tuple[int, ...], dict[str, object]]] = {}
    for runtime in payload.get("runtimes", []):
        if not isinstance(runtime, dict) or not runtime.get("isAvailable", False):
            continue
        identifier = runtime.get("identifier")
        version = runtime.get("version")
        if not isinstance(identifier, str) or not isinstance(version, str):
            continue
        if not identifier.startswith("com.apple.CoreSimulator.SimRuntime.iOS-"):
            continue
        parts = version.split(".")
        if not parts or not all(part.isdigit() for part in parts):
            continue
        numeric = tuple(int(part) for part in parts)
        major = numeric[0]
        if numeric < minimum_parts or (requested and major not in requested):
            continue
        if major not in candidates or numeric > candidates[major][0]:
            candidates[major] = (numeric, runtime)
    missing = requested - candidates.keys()
    if missing:
        raise ValueError("requested Simulator runtimes are unavailable: " +
                         ", ".join(str(major) for major in sorted(missing)))
    if not candidates:
        raise ValueError(f"an available iOS {minimum} or later Simulator runtime is required")
    return [candidates[major][1] for major in sorted(candidates)]


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


def select(runtime_payload: dict[str, object], device_payload: dict[str, object],
           minimum: str, requested: set[int]) -> list[tuple[str, str, str, str]]:
    matrix = []
    for runtime in available_runtimes(runtime_payload, minimum, requested):
        devices = device_payload
        if "supportedDeviceTypes" in runtime:
            supported = {device["identifier"] for device in runtime["supportedDeviceTypes"]}
            devices = {"devicetypes": [device for device in device_payload.get("devicetypes", [])
                                        if device.get("identifier") in supported]}
        matrix.append((
            "iOS-" + str(runtime["version"]).split(".")[0],
            str(runtime["identifier"]),
            preferred_device_type(devices, PHONE_PREFERENCES),
            preferred_device_type(devices, TABLET_PREFERENCES),
        ))
    return matrix


def main() -> int:
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <runtimes.json> <device-types.json> <minimum> [major ...]",
              file=sys.stderr)
        return 2
    try:
        runtime_payload = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
        device_payload = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
        requested = {int(value) for value in sys.argv[4:]}
        for row in select(runtime_payload, device_payload, sys.argv[3], requested):
            print("\t".join(row))
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
