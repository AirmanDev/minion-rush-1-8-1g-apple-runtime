"""Collect dependency metadata and license files for the frozen Windows helper."""

from importlib import metadata
import json
from pathlib import Path
import re
import sys


def notices() -> str:
    parts = ["Third-party dependencies\n"]
    for distribution in sorted(metadata.distributions(), key=lambda item: item.metadata["Name"].casefold()):
        parts.append(f"\n{distribution.metadata['Name']} {distribution.version}\n")
        parts.append("License: " + (distribution.metadata.get("License-Expression")
                                   or distribution.metadata.get("License") or "See license files") + "\n")
        for entry in distribution.files or []:
            name = Path(str(entry)).name.casefold()
            if name.startswith(("license", "licence", "copying", "notice")):
                path = distribution.locate_file(entry)
                if path.is_file():
                    parts.append(f"\n{entry}\n{path.read_text(encoding='utf-8', errors='replace')}\n")
    return "".join(parts)


def dotnet_notices(assets: Path, destination: Path) -> None:
    manifest = json.loads(assets.read_text(encoding="utf-8"))
    packages = Path(manifest["project"]["restore"]["packagesPath"])
    licenses = []
    third_party = []
    seen = set()
    for framework in manifest["project"]["frameworks"].values():
        for dependency in framework.get("downloadDependencies", []):
            name = dependency["name"].lower()
            if not name.startswith(("microsoft.netcore.app.runtime.", "microsoft.windowsdesktop.app.runtime.")):
                continue
            match = re.fullmatch(r"\[([^,]+),\s*\1\]", dependency["version"])
            if not match:
                raise ValueError("The .NET runtime package must have an exact resolved version.")
            identity = (name, match[1])
            if identity in seen:
                continue
            seen.add(identity)
            directory = packages / name / match[1]
            for entry in directory.iterdir():
                if not entry.is_file():
                    continue
                if entry.name.casefold().startswith("license"):
                    licenses.append(f"{name} {match[1]}\n" + entry.read_text(encoding="utf-8-sig"))
                elif entry.name.casefold().startswith("third-party-notices"):
                    third_party.append(f"{name} {match[1]}\n" + entry.read_text(encoding="utf-8-sig"))
    if len(licenses) < 2 or not third_party:
        raise ValueError("Missing .NET Core or Windows Desktop license material.")
    (destination / "DOTNET_LICENSE.txt").write_text("\n\n".join(licenses), encoding="utf-8")
    (destination / "DOTNET_NOTICES.txt").write_text("\n\n".join(third_party), encoding="utf-8")


if __name__ == "__main__":
    Path(sys.argv[1]).write_text(notices(), encoding="utf-8")
    dotnet_notices(Path(sys.argv[2]), Path(sys.argv[1]).parent)
