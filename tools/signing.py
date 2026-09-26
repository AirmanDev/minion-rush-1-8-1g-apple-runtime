#!/usr/bin/env python3
"""Shared signing-input validation for the installer and deployment tools."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


def signing_settings(team: str, bundle: str, *, require_team: bool = True) -> dict[str, str]:
    rules = json.loads((Path(__file__).resolve().parents[1]
                        / "config/installer_ui.json").read_text())["signingRules"]
    if (team or require_team) and not re.fullmatch(rules["team"], team):
        raise ValueError("Team ID must contain 10 uppercase letters or digits.")
    if not re.fullmatch(rules["bundle"], bundle):
        raise ValueError("Enter a valid, unique bundle identifier.")
    settings = {"MR_BUNDLE_ID": bundle}
    if team:
        settings["MR_DEVELOPMENT_TEAM"] = team
    return settings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--team", default="")
    parser.add_argument("--bundle", required=True)
    args = parser.parse_args()
    try:
        signing_settings(args.team, args.bundle, require_team=False)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
