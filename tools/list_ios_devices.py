#!/usr/bin/env python3
"""List paired physical devices from a devicectl JSON result."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True


def paired_physical_devices(
    payload: object, filters: list[str], selected_identifier: str | None = None
) -> list[dict[str, str]]:
    if not isinstance(payload, dict):
        raise ValueError("devicectl result is not a JSON object")
    result = payload.get("result")
    devices = result.get("devices") if isinstance(result, dict) else None
    if not isinstance(devices, list):
        raise ValueError("device list is missing from the devicectl result")

    folded = [value.casefold() for value in filters]
    selected: list[dict[str, str]] = []
    for device in devices:
        if not isinstance(device, dict):
            continue
        properties = device.get("properties")
        if not isinstance(properties, dict):
            continue
        hardware = properties.get("hardware", {})
        connection = properties.get("connection", {})
        state = properties.get("state", {})
        software = properties.get("software", {})
        if (
            not isinstance(hardware, dict)
            or not isinstance(connection, dict)
            or not isinstance(state, dict)
            or not isinstance(software, dict)
            or hardware.get("reality") != "physical"
            or hardware.get("deviceType") not in ("iPhone", "iPad")
            or connection.get("pairingState") != "paired"
        ):
            continue
        udid = hardware.get("udid")
        identifier = device.get("identifier")
        name = state.get("name")
        version = software.get("osVersionNumber", {})
        version = version.get("stringValue", "") if isinstance(version, dict) else ""
        major = re.match(r"(\d+)", version)
        if not major or int(major[1]) < 17:
            continue
        if not all(isinstance(value, str) and value for value in (udid, identifier, name)):
            continue
        if selected_identifier is not None and identifier != selected_identifier:
            continue
        if folded and not any(value in name.casefold() for value in folded):
            continue
        if any(character in udid + identifier for character in "\r\n\t"):
            continue
        name = " ".join(name.split())
        selected.append({"udid": udid, "id": identifier, "name": name, "os": version})
    return sorted(selected, key=lambda item: (item["name"].casefold(), item["udid"]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("result", type=Path)
    parser.add_argument("filters", nargs="*")
    parser.add_argument("--identifier", help="match one exact devicectl identifier")
    args = parser.parse_args()
    try:
        with args.result.open(encoding="utf-8") as source:
            payload = json.load(source)
        devices = paired_physical_devices(payload, args.filters, args.identifier)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        raise SystemExit(f"invalid devicectl result: {exc}") from exc
    for device in devices:
        print("\t".join(device[key] for key in ("udid", "id", "name")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
