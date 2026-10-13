#!/usr/bin/env python3
"""Create only symbolic runtime bindings, never game addresses, for the iOS kit."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from localize import Catalog, generated_header
from common import ROOT
from native_profile import CONFIG, symbol_names


def render(symbols: Path) -> str:
    names = symbol_names(symbols)
    macros = [
        "// Generated from the public symbol ABI; contains no game addresses.",
        "#ifndef MR_GAME_BINDINGS_H",
        "#define MR_GAME_BINDINGS_H",
        "#include \"game_profile.h\"",
        f"#define MR_GAME_SYMBOL_COUNT {len(names)}u",
        "#define MR_GAME_BINDING_HASH (mr_profile_fingerprint)",
        "#define MR_GAME_BLOCKED_FUNCTIONS (mr_profile_blocked)",
        "#define MR_GAME_OFFLINE_HOOK_FUNCTIONS (mr_profile_hooks)",
        "#define MR_GAME_BLOCKED_FUNCTION_COUNT (mr_profile_blocked_count)",
        "#define MR_GAME_OFFLINE_HOOK_FUNCTION_COUNT (mr_profile_hook_count)",
        "#define MR_GAME_EXACT_BLOCKED_FUNCTION_COUNT (mr_profile_exact_blocked_count)",
    ]
    macros.extend(f"#define MR_GAME_{name} (mr_profile_bindings[MR_NATIVE_PROFILE_{name}])" for name in names)
    return "\n".join(macros + ["#endif", ""])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.output.parent.is_dir() or args.output.is_symlink():
        parser.error("output parent must exist and output must not be a symlink")
    args.output.write_text(render(ROOT / "src/native/game_symbols.def"), encoding="ascii")
    configuration = json.loads((ROOT / "config/localizations/hu.json").read_text(encoding="utf-8"))
    public_language = Catalog(
        code=configuration["code"], display_name=configuration["display_name"],
        engine_code=configuration["engine_code"], country=configuration["country"],
        flag_source=configuration["flag"]["source"],
        flag_layout=configuration["flag"]["layout"],
        flag_colors=tuple(int(color, 16) for color in configuration["flag"]["colors"]),
        font_archive=configuration["font"]["archive"],
        font_member=configuration["font"]["member"],
        formatting=configuration["format"], path=Path("config/localizations/hu.json"),
    )
    (args.output.parent / "localization_config.h").write_bytes(generated_header([public_language]))
    limits = ["// Generated from config/native_profile.json.",
              "#ifndef MR_GAME_PROFILE_LIMITS_H", "#define MR_GAME_PROFILE_LIMITS_H"]
    limits += [f"#define MR_AOT_{key.upper()} {value}u" for key, value in
               (("code_slot_bytes", CONFIG["code_slot_bytes"]),
                ("max_blocks", CONFIG["max_blocks"]),
                ("max_blocked", CONFIG["max_blocked"]),
                ("max_hooks", CONFIG["max_hooks"]))]
    (args.output.parent / "game_profile_limits.h").write_text(
        "\n".join(limits + ["#endif", ""]), encoding="ascii")


if __name__ == "__main__":
    main()
