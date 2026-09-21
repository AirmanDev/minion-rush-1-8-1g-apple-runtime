#!/usr/bin/env python3
"""List paired physical devices from a devicectl JSON result."""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True


def paired_physical_devices(
    payload: object, filters: list[str]
) -> list[tuple[str, str, str]]:
    if not isinstance(payload, dict):
        raise ValueError("devicectl result is not a JSON object")
    result = payload.get("result")
    devices = result.get("devices") if isinstance(result, dict) else None
    if not isinstance(devices, list):
        raise ValueError("device list is missing from the devicectl result")

    folded = [value.casefold() for value in filters]
    selected: list[tuple[str, str, str]] = []
    for device in devices:
        if not isinstance(device, dict):
            continue
        hardware = device.get("hardwareProperties", {})
        connection = device.get("connectionProperties", {})
        properties = device.get("deviceProperties", {})
        if (
            not isinstance(hardware, dict)
            or not isinstance(connection, dict)
            or not isinstance(properties, dict)
            or hardware.get("reality") != "physical"
            or connection.get("pairingState") != "paired"
        ):
            continue
        udid = hardware.get("udid")
        identifier = device.get("identifier")
        name = properties.get("name")
        if not all(isinstance(value, str) and value for value in (udid, identifier, name)):
            continue
        if folded and not any(value in name.casefold() for value in folded):
            continue
        selected.append((udid, identifier, name))
    return sorted(selected, key=lambda item: (item[2].casefold(), item[0]))


def main() -> int:
    if len(sys.argv) < 2:
        raise SystemExit("usage: list_ios_devices.py <devices.json> [name-filter ...]")
    try:
        with Path(sys.argv[1]).open(encoding="utf-8") as source:
            payload = json.load(source)
        devices = paired_physical_devices(payload, sys.argv[2:])
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        raise SystemExit(f"invalid devicectl result: {exc}") from exc
    for device in devices:
        print("\t".join(device))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
