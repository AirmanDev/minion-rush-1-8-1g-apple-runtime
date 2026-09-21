#!/usr/bin/env python3
"""Generate the one supported graphics configuration."""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

sys.dont_write_bytecode = True

from common import ROOT, load_json_object

CONFIG_PATH = ROOT / "config" / "graphics.json"
# The engine reads and rewrites profiles.json as latin-1; naming it keeps the
# generated file byte-comparable with the one that shipped.
ENCODING = "latin-1"


def numbered_keys(profiles: dict, prefix: str) -> list[str]:
    keys = sorted(key for key in profiles if key.startswith(prefix))
    if not keys:
        raise ValueError(f"source has no {prefix}* profile")
    return keys


def validated_profiles(config: dict, source: dict) -> dict:
    if set(config) != {"host", "game"}:
        raise ValueError("graphics.json root keys must be host and game")
    host = config["host"]
    game = config["game"]
    if set(host) != {"render_scale", "msaa_samples", "anisotropy", "pbo", "engine_hz"}:
        raise ValueError("invalid host graphics configuration")
    if set(game) != {"cpu", "gpu", "texture_budget_mb"}:
        raise ValueError("invalid game-engine graphics configuration")

    scale = host["render_scale"]
    samples = host["msaa_samples"]
    anisotropy = host["anisotropy"]
    if not isinstance(scale, (int, float)) or not math.isfinite(scale) or not 1 <= scale <= 2:
        raise ValueError("render_scale must be a number from 1.0 to 2.0")
    if not isinstance(samples, int) or samples not in {0, 2, 4, 8}:
        raise ValueError("msaa_samples must be 0, 2, 4, or 8")
    if not isinstance(anisotropy, (int, float)) or anisotropy not in {1, 2, 4, 8, 16}:
        raise ValueError("anisotropy must be 1, 2, 4, 8, or 16")
    if not isinstance(host["pbo"], bool):
        raise ValueError("pbo must be a Boolean")
    # The engine uses trunc(1000 / engine_hz) milliseconds for both its time
    # step and importer gate. The supported rates are the stock 30 Hz and the
    # engine's own k_updateTime rate of 60 Hz. Rendering is capped at 60 Hz, so
    # a 120 Hz simulation would only add work and energy use between frames.
    if host["engine_hz"] not in {30, 60}:
        raise ValueError("engine_hz must be 30 or 60")
    if not isinstance(game["texture_budget_mb"], int) or game["texture_budget_mb"] < 128:
        raise ValueError("texture_budget_mb must be at least 128")

    profiles = source.get("Profiles")
    if not isinstance(profiles, dict):
        raise ValueError("Profiles object is missing")
    gpu_reference = profiles.get("GPU_5")
    cpu_reference = profiles.get("CPU_2")
    if not isinstance(gpu_reference, dict) or not isinstance(cpu_reference, dict):
        raise ValueError("GPU_5 or CPU_2 reference is missing")
    if set(game["gpu"]) != set(gpu_reference):
        missing = sorted(set(gpu_reference) - set(game["gpu"]))
        extra = sorted(set(game["gpu"]) - set(gpu_reference))
        raise ValueError(f"GPU key set differs; missing={missing}, extra={extra}")
    if set(game["cpu"]) != set(cpu_reference):
        raise ValueError("CPU key set differs from the engine schema")

    # Only genuine distance controls may exceed the highest stock profile.
    # Enumerations and switches must use values already supported by the game.
    extendable = {
        "backgroundFarPlane",
        "synchronizedObjectDistance",
        "decosAnimationRadius",
    }
    gpu_profiles = [profiles[key] for key in numbered_keys(profiles, "GPU_")]
    for key, value in game["gpu"].items():
        if key in extendable:
            continue
        supported = {profile[key] for profile in gpu_profiles}
        if value not in supported:
            raise ValueError(f"unsupported GPU value: {key}={value}")

    cpu_profiles = [profiles[key] for key in numbered_keys(profiles, "CPU_")]
    for key, value in game["cpu"].items():
        supported = {profile[key] for profile in cpu_profiles}
        if value not in supported:
            raise ValueError(f"unsupported CPU value: {key}={value}")

    return profiles


def configured_values(config: dict) -> dict[str, dict]:
    game = config["game"]
    return {
        "CPU_": game["cpu"],
        "GPU_": game["gpu"],
        "MEM_": {"textureBudget": game["texture_budget_mb"]},
    }


def apply_configuration(config: dict, source: dict) -> dict:
    profiles = validated_profiles(config, source)
    for prefix, value in configured_values(config).items():
        for key in numbered_keys(profiles, prefix):
            profiles[key] = dict(value)
    return source


def validate(config: dict, source: dict) -> None:
    profiles = validated_profiles(config, source)

    for prefix, value in configured_values(config).items():
        different = [
            key for key in numbered_keys(profiles, prefix)
            if profiles[key] != value
        ]
        if different:
            raise ValueError(
                "profiles.json differs from the fixed configuration: "
                + ", ".join(different)
            )


def header(config: dict) -> str:
    host = config["host"]
    return (
        "/* Generated by tools/configure_graphics.py; do not edit manually. */\n"
        "#ifndef MR_GRAPHICS_CONFIG_H\n"
        "#define MR_GRAPHICS_CONFIG_H\n\n"
        f"#define MR_GRAPHICS_RENDER_SCALE {float(host['render_scale']):.6f}\n"
        f"#define MR_GRAPHICS_MSAA_SAMPLES {int(host['msaa_samples'])}\n"
        f"#define MR_GRAPHICS_ANISOTROPY {float(host['anisotropy']):.6f}f\n"
        f"#define MR_GRAPHICS_USE_PBO {1 if host['pbo'] else 0}\n"
        f"#define MR_GRAPHICS_ENGINE_HZ {int(host['engine_hz'])}\n\n"
        "#endif\n"
    )


def write_profiles(path: Path, profiles: dict) -> None:
    path.write_text(
        json.dumps(profiles, ensure_ascii=False, separators=(",", ":")),
        encoding=ENCODING,
    )


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: configure_graphics.py <profiles.json> <output.h>"
        )
    source_path, output_header = map(Path, sys.argv[1:])
    try:
        config = load_json_object(CONFIG_PATH)
        source = load_json_object(source_path, ENCODING)
    except (OSError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc
    validate(config, source)
    output_header.write_text(header(config), encoding="ascii")


if __name__ == "__main__":
    main()
