#!/usr/bin/env python3
"""Read-only validation for the maintained source and project structure."""

from __future__ import annotations

import hashlib
import plistlib
import re
import sys
from collections import defaultdict
from pathlib import Path

sys.dont_write_bytecode = True

from common import ROOT, content_files, load_json_object

IGNORED_ROOTS = frozenset({
    "assets", "build", "logs", ".firecrawl", ".git", "__MACOSX", "__pycache__",
    "xcuserdata",
})
SEARCHABLE_SUFFIXES = {
    ".c", ".h", ".m", ".swift", ".py", ".sh", ".md", ".S", ".def", ".json",
    ".pbxproj", ".xcprivacy", ".xcworkspacedata",
}


def load_source_manifest() -> frozenset[str]:
    manifest = ROOT / "config/source_manifest.txt"
    try:
        lines = manifest.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise SystemExit(f"source manifest is missing: {manifest}") from exc

    entries = [line.strip() for line in lines if line.strip() and not line.startswith("#")]
    if entries != sorted(entries):
        raise SystemExit("source manifest is not sorted")
    if len(entries) != len(set(entries)):
        raise SystemExit("duplicate entry in source manifest")
    for entry in entries:
        path = Path(entry)
        if path.is_absolute() or ".." in path.parts or entry.startswith("./"):
            raise SystemExit(f"invalid source manifest entry: {entry}")
    if "config/source_manifest.txt" not in entries:
        raise SystemExit("source manifest does not include itself")
    return frozenset(entries)


EXPECTED = load_source_manifest()
FORBIDDEN_TEXT = {
    "MR_INTERP_PROFILE",
    "MR_A64_SEEDS",
    "MR_MISS_LOG",
    "MR_A64_UNRESOLVED",
    "MR_PROFILE",
    "MR_GAME_FILES",
    "MR_RENDER_SCALE",
    "MR_MSAA",
    "MR_AF",
    "MR_PBO",
    "graphics-presets",
    "generate_profiles",
    "profiles-ultra",
    "profiles-balanced",
    "profiles-speed",
    "ultra|balanced|speed",
    "speed_gpu_overrides",
    "ultra_gpu_overrides",
    "CGLFlushDrawable",
    "unresolved-blocks.tsv",
    "guest_swaps",
    "vendeg-swap",
    "mr_sched_set_frame_work",
    "FRAME_BACKGROUND_WORK",
    "FN_GAME_API_FIRST",
    "FN_GAME_API_END",
    "remove_transient_files",
    "clean_code_metadata",
    "MR_A64_MAX_CHAIN",
    "FIXED_HOOKS",
    "atomic_thread_fence",
    "ensure_build",
    "needs_rebuild",
    "MR_MAX_GUARDS",
    "WAIT_GUARD",
    "mr_sched_guard_",
    "MR_LOG_AUDIO",
    "MR_REQUIRE_OFFLINE_EVENT",
    "MR_REQUIRE_AUDIO_SIGNAL",
    "MR_WAIT_MAIN_MENU_AUDIO",
    "MR_TEST_TIMEOUT_MS",
    "MR_DIRECT_MMAP",
    "mr_mmap_init",
    "MMAP_POOL",
}
# Exactly-once invariants: each of these marks a single implementation of
# something the project refuses to have two of.
SINGLETONS = {
    "mr_gl_blit_to_window((uint32_t)": "exactly one host present is required per platform",
    '"eglSwapBuffers", t_eglSwapBuffers': "exactly one Android EGL swap import is required",
    "PRESENT_COUNT++": "exactly one runtime host-present counter is required",
    "mr_offline_before_block(cpu)": "exactly one offline runtime hook is required",
    "mr_guest_clock_step(timing.refresh_ms": "exactly one site may advance the frame timeline",
    "mr_guest_clock_monotonic(c, &ns)": "guest monotonic time must have one source",
    "mr_guest_clock_realtime(c, &ns)": "guest real time must have one source",
    "mr_guest_block_entry(cpu)": "exactly one block-entry hook is required",
    "MR_GAME_FRAME_UPDATE": "exactly one site may observe simulation updates",
    "game + GAME_TARGET_FPS_OFFSET": "exactly one site may write the engine target rate",
    "mr_guest_set_target_fps(MR_GRAPHICS_ENGINE_HZ)": "target rate must have one source",
}
# Mutually exclusive platform alternatives. A build links exactly one member
# from each group. Grouping matters because an invariant may apply to the
# window layer but not the audio-session layer.
PLATFORM_ALTERNATIVES = (
    ("src/native/window_macos.m", "src/native/window_ios.m"),
    ("src/native/audio_session_macos.c", "src/native/audio_session_ios.m"),
)
PLATFORM_FILES = frozenset(
    path for group in PLATFORM_ALTERNATIVES for path in group
)
RUNTIME_SOURCES = (
    "src/native/main.c",
    "src/native/offline_mode.c",
    "src/native/offline_events.c",
)


def read_tree() -> dict[str, str]:
    """Every maintained file, keyed by its path relative to the project root."""
    tree: dict[str, str] = {}
    for path in content_files(ROOT, IGNORED_ROOTS):
        relative = str(path.relative_to(ROOT))
        if relative not in EXPECTED and path.suffix not in SEARCHABLE_SUFFIXES:
            continue
        if path.is_symlink():
            raise SystemExit(f"symbolic link in source tree: {relative}")
        blob = path.read_bytes()
        try:
            tree[relative] = blob.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise SystemExit(f"source file is not UTF-8: {relative}") from exc
    return tree


def normalized_functions(relative: str, source: str) -> list[tuple[str, str]]:
    """Every function body of a C/Objective-C file, whitespace and comments out."""
    if not relative.endswith((".c", ".m")):
        return []
    found: list[tuple[str, str]] = []
    depth = 0
    body_start: int | None = None
    signature = ""
    i = 0
    while i < len(source):
        if source.startswith("//", i):
            end = source.find("\n", i)
            i = len(source) if end < 0 else end + 1
            continue
        if source.startswith("/*", i):
            end = source.find("*/", i + 2)
            if end < 0:
                raise SystemExit(f"unterminated comment: {relative}")
            i = end + 2
            continue
        char = source[i]
        if char in {'"', "'"}:
            quote = char
            i += 1
            while i < len(source):
                if source[i] == "\\":
                    i += 2
                    continue
                if source[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if char == "{":
            if depth == 0:
                boundary = max(
                    source.rfind(";", 0, i),
                    source.rfind("}", 0, i),
                    source.rfind("\n\n", 0, i),
                )
                signature = source[boundary + 1 : i].strip()
                if "(" in signature and not re.match(r"^(if|for|while|switch)\b", signature):
                    body_start = i
            depth += 1
        elif char == "}":
            depth -= 1
            if depth < 0:
                raise SystemExit(f"unmatched closing brace: {relative}")
            if depth == 0 and body_start is not None:
                body = source[body_start + 1 : i]
                body = re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S)
                body = re.sub(r"\s+", "", body)
                if len(body) > 80:
                    found.append((signature, body))
                body_start = None
        i += 1
    if depth != 0:
        raise SystemExit(f"unbalanced braces: {relative}")
    return found


def check_tree_shape(tree: dict[str, str]) -> None:
    missing = sorted(EXPECTED - set(tree))
    unexpected = sorted(set(tree) - EXPECTED)
    if missing or unexpected:
        details = []
        if missing:
            details.append("missing: " + ", ".join(missing))
        if unexpected:
            details.append("unexpected: " + ", ".join(unexpected))
        raise SystemExit("inconsistent source tree; " + "; ".join(details))
    for relative in sorted(path for path in tree if path.endswith(".sh")):
        if relative == "tools/project_common.sh":
            continue
        if not (ROOT / relative).stat().st_mode & 0o111:
            raise SystemExit(f"{relative} is not executable")
    if (ROOT / "tools/project_common.sh").stat().st_mode & 0o111:
        raise SystemExit("internal project_common.sh must not be executable")

    icon = ROOT / "ios/MinionRush/Assets.xcassets/AppIcon.appiconset/AppIcon.png"
    if icon.exists():
        raise SystemExit("generated or copyrighted app icon remains in the source tree")

def check_python_syntax(tree: dict[str, str]) -> None:
    for relative, source in tree.items():
        if relative.endswith(".py"):
            try:
                compile(source, relative, "exec")
            except SyntaxError as exc:
                raise SystemExit(f"invalid Python source: {relative}: {exc}") from exc


def check_text_style(tree: dict[str, str]) -> None:
    found = sorted(relative for relative, source in tree.items() if not source.isascii())
    if found:
        raise SystemExit("non-ASCII text in maintained source: " + ", ".join(found))


def check_public_repository(tree: dict[str, str]) -> None:
    ignored = set(tree[".gitignore"].splitlines())
    required_ignores = {
        "/assets/", "/build/", "/localizations/", "/logs/", "xcuserdata/",
    }
    missing_ignores = sorted(required_ignores - ignored)
    if missing_ignores:
        raise SystemExit("not excluded from public repository: " + ", ".join(missing_ignores))
    if any(path.startswith("localizations/") for path in EXPECTED):
        raise SystemExit("proprietary localization text is part of the public source manifest")

    public_docs = "\n".join(
        tree[path]
        for path in ("README.md", "CONTRIBUTING.md", "SECURITY.md", "docs/LEGAL.md")
    )
    for token in ("1.8.1g", "Unity", "game files"):
        if token not in public_docs:
            raise SystemExit(f"incomplete public scope: {token}")

    license_text = tree["LICENSE"]
    license_markers = (
        "Apache License\n                           Version 2.0, January 2004",
        "2. Grant of Copyright License.",
        "3. Grant of Patent License.",
        "END OF TERMS AND CONDITIONS",
    )
    if any(marker not in license_text for marker in license_markers):
        raise SystemExit("LICENSE is not the complete Apache License 2.0 text")
    if "Apache License 2.0" not in tree["README.md"] or \
       "Apache License 2.0" not in tree["CONTRIBUTING.md"] or \
       "Apache\nLicense 2.0" not in tree["docs/LEGAL.md"]:
        raise SystemExit("Apache-2.0 project scope is not documented consistently")

    workflow = tree[".github/workflows/source-checks.yml"]
    for token in (
        "permissions:\n  contents: read",
        "python3 -m unittest discover -s tests -v",
        "python3 tools/validate_source.py",
        "bash -n ./*.sh tools/*.sh",
        "persist-credentials: false",
    ):
        if token not in workflow:
            raise SystemExit("incomplete GitHub source checks")
    actions = re.findall(r"\buses:\s*[^@\s]+@([^\s#]+)", workflow)
    if not actions or any(not re.fullmatch(r"[0-9a-f]{40}", ref) for ref in actions):
        raise SystemExit("GitHub Action is not pinned to a full commit SHA")

    common = tree["tools/project_common.sh"]
    installer = tree["tools/install_assets.py"]
    asset_validator = tree["tools/validate_assets.py"]
    icon_preparer = tree["tools/prepare_app_icon.swift"]
    icon_contract = (
        'APP_ICON_IN_APK = "res/drawable-xxxhdpi/icon.png"',
        "OFFICIAL_APP_ICON_SHA256",
        'APP_ICON_SOURCE="$ASSETS_ROOT/app-icon.png"',
        'APP_ICON_PREPARER="$PROJECT_ROOT/tools/prepare_app_icon.swift"',
        "CGColorSpace.sRGB",
        "CGImageAlphaInfo.noneSkipLast",
        "private let outputSize = 1024",
        "private func isOuterRed",
        "project_prepare_ios_app_icon()",
        "project_remove_ios_app_icon()",
        "project_verify_ios_app_icon()",
    )
    icon_sources = installer + asset_validator + icon_preparer + common
    if any(token not in icon_sources for token in icon_contract) or \
       "/ios/MinionRush/Assets.xcassets/AppIcon.appiconset/AppIcon.png" not in tree[".gitignore"]:
        raise SystemExit("private application-icon pipeline is incomplete")
    if "write_profiles(profiles_path, apply_configuration(config, profiles))" not in installer:
        raise SystemExit("asset installation does not apply the graphics configuration")
    format_tool = tree["tools/format_source.sh"]
    if "clang-format" not in format_tool or "swift-format" not in format_tool or \
       "--dry-run" not in format_tool or "lint --strict" not in format_tool or \
       "./tools/format_source.sh --check" not in tree["CONTRIBUTING.md"]:
        raise SystemExit("native source formatting cannot be checked with one command")

    public_tree = {
        relative: source
        for relative, source in tree.items()
        if relative != "tools/validate_source.py"
    }
    private_markers = ("/Users/", "com.airmancooma", "DEVELOPMENT_TEAM = KT")
    leaked = sorted(
        relative for relative, source in public_tree.items()
        if any(marker in source for marker in private_markers)
    )
    if leaked:
        raise SystemExit("personal developer data remains: " + ", ".join(leaked))


def check_no_duplication(tree: dict[str, str]) -> None:
    by_content: dict[str, list[str]] = defaultdict(list)
    by_body: dict[str, list[str]] = defaultdict(list)
    for relative, source in tree.items():
        by_content[hashlib.sha256(source.encode()).hexdigest()].append(relative)
        for signature, body in normalized_functions(relative, source):
            by_body[hashlib.sha256(body.encode()).hexdigest()].append(
                f"{relative}: {signature}"
            )
    duplicate_files = [paths for paths in by_content.values() if len(paths) > 1]
    if duplicate_files:
        raise SystemExit(f"files with identical content: {duplicate_files}")
    duplicate_functions = [items for items in by_body.values() if len(items) > 1]
    if duplicate_functions:
        raise SystemExit(f"duplicate function bodies: {duplicate_functions}")


def check_graphics_config() -> None:
    """One fixed, top-quality configuration -- nobody may quietly downgrade it.

    The shape and the engine-supported value ranges are the generator's
    business (tools/configure_graphics.py); this is the quality floor.
    """
    config = load_json_object(ROOT / "config/graphics.json")
    if set(config) != {"host", "game"}:
        raise SystemExit("graphics.json does not contain one host/game configuration")
    host = config["host"]
    gpu = config["game"]["gpu"]
    cpu = config["game"]["cpu"]
    if gpu.get("backgroundFarPlane", 0) < 800:
        raise SystemExit("fixed graphics configuration has insufficient draw distance")
    if gpu.get("hideOptionalDecos") != 0:
        raise SystemExit("fixed graphics configuration hides decorations")
    if host.get("render_scale", 0) < 1.25 or host.get("msaa_samples", 0) < 2:
        raise SystemExit("fixed host graphics are below the quality floor")
    if host.get("anisotropy", 0) < 8:
        raise SystemExit("fixed anisotropic filtering is below the quality floor")
    # Simulation rate is not a quality setting, so it has no minimum here.
    # The engine has no render interpolation and gameplay timing was designed
    # around a 33 ms step. Support the stock 30 Hz and the engine's own 60 Hz
    # k_updateTime value, which also matches the presentation ceiling.
    if host.get("engine_hz") not in {30, 60}:
        raise SystemExit("fixed simulation rate is not 30 or 60 Hz")
    if gpu.get("useShadows") != 3 or gpu.get("usePostFX") != 1:
        raise SystemExit("fixed graphics do not use the supported maximum quality")
    if gpu.get("useLowModels") != 0 or gpu.get("useLowMaterials") != 0:
        raise SystemExit("fixed graphics use low-quality assets")
    if cpu.get("useLessSounds") != 0 or cpu.get("useLessBananaSounds") != 0:
        raise SystemExit("fixed graphics configuration reduces sound playback")


def check_single_solutions(tree: dict[str, str]) -> None:
    searchable = "\n".join(
        source
        for relative, source in tree.items()
        if Path(relative).suffix in SEARCHABLE_SUFFIXES
        and relative != "tools/validate_source.py"
    )
    stale = sorted(token for token in FORBIDDEN_TEXT if token in searchable)
    if stale:
        raise SystemExit("obsolete or parallel reference: " + ", ".join(stale))
    # Platform alternatives are mutually exclusive: a build links one member
    # from each group. Exactly-once invariants therefore describe a linked
    # binary, not the complete source tree.
    per_platform = "\n".join(
        source
        for relative, source in tree.items()
        if relative not in PLATFORM_FILES
        and Path(relative).suffix in SEARCHABLE_SUFFIXES
        and relative != "tools/validate_source.py"
    )
    for token, message in SINGLETONS.items():
        if searchable.count(token) == 1:
            continue
        # Once per platform: absent from common code, present in every member
        # of exactly one alternative group, and absent from the other groups.
        if per_platform.count(token) == 0 and any(
            all(tree[alt].count(token) == 1 for alt in group)
            and all(tree[other].count(token) == 0
                    for other in PLATFORM_FILES - set(group))
            for group in PLATFORM_ALTERNATIVES
        ):
            continue
        raise SystemExit(message)


def check_build_time_bindings(tree: dict[str, str]) -> None:
    """No engine address and no symbol name may be resolved at runtime."""
    for relative in ("src/native/offline_mode.c", "src/native/offline_events.c"):
        if re.search(r"\b0x0[0-9a-fA-F]{7}u?\b", tree[relative]):
            raise SystemExit(f"hard-coded engine address in offline system: {relative}")
    runtime = "\n".join(tree[relative] for relative in RUNTIME_SOURCES)
    if "mr_elf_lookup(" in runtime or "mr_elf_each_named_function(" in runtime:
        raise SystemExit("runtime game-symbol lookup remains in source")
    if "game_bindings.h" not in runtime:
        raise SystemExit("runtime does not use build-time game bindings")


def check_architecture(tree: dict[str, str]) -> None:
    """Reject parallel runtime paths; builds/tests verify implementation details."""
    symbols = tree["src/native/game_symbols.def"]
    threads = tree["src/native/guest_threads.c"]
    main = tree["src/native/main.c"]
    windows = (
        tree["src/native/window_macos.m"],
        tree["src/native/window_ios.m"],
    )
    gl = tree["src/native/shim_gl.c"]
    audio = tree["src/native/shim_audio.c"]
    libc = tree["src/native/shim_libc.c"]
    language_ui = tree["src/native/language_ui.c"]
    offline = tree["src/native/offline_mode.c"]
    safe_area = tree["src/native/safe_area.c"]
    localization_tool = tree["tools/localize.py"]
    test = tree["test.sh"]
    timeout = tree["tools/run_with_timeout.py"]
    run = tree["run.sh"]

    if symbols.count("MR_GAME_HOOK_SYMBOL(") < 1:
        raise SystemExit("declarative offline-hook list is missing")

    native_without_guest_threads = "\n".join(
        source for relative, source in tree.items()
        if relative.startswith("src/native/") and relative != "src/native/guest_threads.c"
    )
    if threads.count('"pthread_create", s_pthread_create') != 1 or \
       re.search(r"\bpthread_create\s*\(", native_without_guest_threads):
        raise SystemExit("second host-thread guest execution path remains")

    if main.count("mr_win_enable_input();") != 1 or any(
        window.count("void mr_win_enable_input(void)") != 1 for window in windows
    ):
        raise SystemExit("startup input gate is missing or duplicated")

    if "MR_STARTUP_TEST_MS" not in test or "MR_DIAGNOSTICS=1" not in test:
        raise SystemExit("startup test does not use the single diagnostics path")
    if test.count("tools/run_with_timeout.py") != 1 or \
       "process.terminate()" not in timeout or "process.kill()" not in timeout:
        raise SystemExit("windowed startup test has no external TERM/KILL timeout")
    if "MR_DIAGNOSTICS=1" not in run:
        raise SystemExit("debug run does not use the single diagnostics mode")
    for token in ("mr_offline_main_menu_ready()", "mr_offline_events_ready()",
                  "audio.signal_callbacks > startup_signal_callbacks"):
        if token not in main:
            raise SystemExit("startup-test condition is incomplete")
    if "ERROR: guest attempted a network import during startup" not in main:
        raise SystemExit("startup test does not reject guest network activity")

    if "UPLOAD_PBO_COUNT 3u" not in gl or "glFenceSync" not in gl:
        raise SystemExit("stable three-PBO upload path is incomplete")
    rgb_read = re.search(
        r"glReadPixels\s*\([^;]*\bGL_RGB\b\s*,\s*GL_UNSIGNED_BYTE", gl,
        re.DOTALL,
    )
    rgba_read = re.search(
        r"glReadPixels\s*\([^;]*\bGL_RGBA\b\s*,\s*GL_UNSIGNED_BYTE\s*,\s*data\s*\)",
        gl,
        re.DOTALL,
    )
    if "read_framebuffer_rgb" not in gl or rgb_read or not rgba_read:
        raise SystemExit("framebuffer readback does not use the portable RGBA path")
    if "memcpy(tmp, mr_mem(c, sp), (size_t)len);" not in gl or \
       "memcpy(tmp, raw, (size_t)len);" in gl:
        raise SystemExit("length-delimited shader source is not copied from guest memory")

    # Host time has one source. Scheduler deadlines use real time while render
    # work observes a display-quantized frame timeline.
    host_clock_users = [
        relative for relative, source in tree.items()
        if relative.startswith("src/native/")
        and relative != "src/native/host_time.h"
        and re.search(r"\bclock_gettime\s*\(\s*CLOCK_", source)
    ]
    if host_clock_users:
        raise SystemExit(
            "direct host-clock read outside host_time.h: "
            + ", ".join(sorted(host_clock_users)))
    if "mr_guest_clock_begin_render();" not in main or \
       "mr_guest_clock_end_render();" not in main:
        raise SystemExit("render window does not mark the frame timeline")
    if "pacing_observe(&pacing" not in main:
        raise SystemExit("engine simulation rate is not measured")
    for token in ("STEP_RESIDUAL", "1000.0 / (double)TARGET_FPS_BYTE", "apply_time_step(cpu)"):
        if token not in tree["src/native/guest_runtime.c"]:
            raise SystemExit("fractional simulation step is incomplete: " + token)
    if "MR_DIAGNOSTICS" not in audio or \
       "kAudioUnitRenderAction_OutputIsSilence" not in audio:
        raise SystemExit("diagnostic PCM validation is incomplete")
    if "pread((int)fd" not in libc or "heap_alloc(c, len)" not in libc:
        raise SystemExit("stable guest mmap-data lifetime is incomplete")
    network_integer_imports = (
        "accept", "bind", "connect", "gethostname", "getpeername", "getsockname",
        "getsockopt", "listen", "poll", "recv", "recvfrom", "recvmsg", "select",
        "send", "sendmsg", "sendto", "setsockopt", "shutdown", "socket",
    )
    if any(f'{{"{name}", s_network_offline}}' not in libc
           for name in network_integer_imports) or any(
               f'{{"{name}", s_network_pointer_offline}}' not in libc
               for name in ("gethostbyaddr", "gethostbyname", "inet_ntoa")
           ) or '{"inet_addr", s_inet_addr_offline}' not in libc or \
           '{"getaddrinfo", s_getaddrinfo_offline}' not in libc or \
           '{"freeaddrinfo", s_freeaddrinfo_offline}' not in libc:
        raise SystemExit("guest network imports are not all fail-closed")
    offline_state = (
        "pc == API.has_internet || pc == API.can_login || pc == API.is_logged_in"
    )
    if offline_state not in offline or "mr_guest_synthetic_return(cpu, 0)" not in offline:
        raise SystemExit("retired online services do not report a consistent offline state")
    if 'MR_BLOCKED_SYMBOL("_ZN16ConnectionPoller9ThreadRunEv")' not in symbols:
        raise SystemExit("retired connection-poller worker is not disabled")
    if 'MR_BLOCKED_SYMBOL("_ZN6glotv39DNSClient15tryToResolveDNSEv")' not in symbols:
        raise SystemExit("retired DNS worker is not disabled")
    for token in ("arrange_trailing_row", "centered_column", "rounded_slices", "create_flag",
                  "FLAG_INACTIVE_TINT", "update_flag_styles"):
        if token not in language_ui:
            raise SystemExit("community-language UI contract is incomplete: " + token)
    if 'classify_ui_object("Support_loading") != UI_BLOCKED' not in offline:
        raise SystemExit("retired Customer Care control is not covered by the offline UI policy")
    if "MR_GAME_MENU_GET_GRAPH" not in offline or "MR_GAME_BASIC_PAGE_GET_GRAPH" not in offline:
        raise SystemExit("offline UI policy does not cover both engine page hierarchies")
    for token in ("MR_SAFE_AREA_BOTTOM_CONTROL", "MR_SAFE_AREA_BOTTOM_OVERFLOW",
                  "MR_SAFE_AREA_LAYOUT_GROUP", "Common_Score_Value",
                  "mr_win_safe_area()", "bottom_inset",
                  "BOTTOM_CONTROL_CLEARANCE_WIDTH_RATIO", "update_overflow_groups"):
        if token not in safe_area:
            raise SystemExit("safe-area edge policy is incomplete: " + token)
    if "mr_safe_area_register_ui_object" not in offline:
        raise SystemExit("UI lookup path does not register safe-area object roles")
    for token in ("FLAG_LAYOUTS", "FLAG_COLOR_LIMIT", "MR_LOCALIZATION_FLAG_RECTANGLE_CAPACITY"):
        if token not in localization_tool:
            raise SystemExit("data-driven community-flag compiler is incomplete: " + token)


# Platform-bound implementations. Every other src/native file is portable C
# and may use neither an Apple framework nor Objective-C. Enforcing this
# boundary keeps the shared runtime independent from the iOS and macOS shells.
PLATFORM_SOURCES = {
    "src/native/window_macos.m",        # AppKit, CVDisplayLink, NSOpenGL
    "src/native/window_ios.m",          # UIKit, CADisplayLink, touch, CoreMotion
    "src/native/window_ios.h",          # UIKit scene-lifecycle contract
    "src/native/shim_gl.c",             # GL headers through gl_platform.h
    "src/native/gl_platform.h",         # Platform-specific GL namespace
    "src/native/gl_context_macos.c",    # CGL context lifecycle
    "src/native/gl_context_ios.m",      # EAGL context lifecycle
    "src/native/shim_audio.c",          # CoreAudio output AudioUnit
    "src/native/audio_session_ios.m",   # AVAudioSession
}
APPLE_FRAMEWORKS = re.compile(
    r"#\s*(?:import|include)\s*<(?:Cocoa|Carbon|AppKit|UIKit|Foundation|"
    r"CoreFoundation|OpenGL|OpenGLES|QuartzCore|Metal|AudioToolbox|"
    r"AudioUnit|AVFoundation|CoreMotion|CoreAudio)/")


def check_platform_boundary(tree: dict[str, str]) -> None:
    """The portable core may not include platform-specific headers."""
    leaked = []
    for relative, source in tree.items():
        if not relative.startswith("src/native/"):
            continue
        if relative in PLATFORM_SOURCES:
            continue
        if relative.endswith(".m"):
            leaked.append(f"{relative} (Objective-C in portable core)")
        elif APPLE_FRAMEWORKS.search(source):
            leaked.append(f"{relative} (Apple framework)")
    if leaked:
        raise SystemExit("platform-specific code in portable core: "
                         + ", ".join(sorted(leaked)))
    missing = sorted(PLATFORM_SOURCES - set(tree))
    if missing:
        raise SystemExit("missing platform implementation: " + ", ".join(missing))
    # The window contract itself must remain platform-independent.
    contract = tree["src/native/platform_window.h"]
    if APPLE_FRAMEWORKS.search(contract) or "NS" in contract.replace("NSZ", ""):
        raise SystemExit("platform_window.h contract became platform-specific")

    host_source = "\n".join(
        source for relative, source in tree.items()
        if relative.startswith(("src/native/", "ios/"))
    )
    network_frameworks = (
        "CFNetwork", "Network.framework", "SystemConfiguration", "WebKit",
        "NSURLConnection", "NSURLSession", "NWConnection", "WKWebView",
    )
    host_calls = re.compile(
        r"\b(?:accept|bind|connect|getaddrinfo|listen|recv|send|socket)\s*\("
    )
    if any(token in host_source for token in network_frameworks) or host_calls.search(host_source):
        raise SystemExit("host networking API or framework remains in the application")


def check_command_separation(tree: dict[str, str]) -> None:
    """Every shell entry point has one job and no compatibility dispatcher."""
    build = tree["build.sh"]
    run = tree["run.sh"]
    test = tree["test.sh"]
    clean = tree["clean.sh"]
    common = tree["tools/project_common.sh"]
    deploy = tree["tools/deploy_ios.sh"]
    package = tree["tools/package_ios.sh"]
    ios_build = tree["tools/build_ios.sh"]
    readme = tree["README.md"]

    if '"${GAME[@]}"' in build or "MR_WINDOW=" in build:
        raise SystemExit("build.sh launches the game")
    if any(token in run for token in ("$CC", "a64-compiler", "configure_graphics.py")):
        raise SystemExit("run.sh contains compilation logic")
    if any(token in test for token in ("$CC", "a64-compiler", "configure_graphics.py")):
        raise SystemExit("test.sh contains compilation logic")
    if any(token in clean for token in ("$CC", '"${GAME[@]}"', "validate_")):
        raise SystemExit("clean.sh does work other than cleanup")
    if common.count("GAME=(") != 1:
        raise SystemExit("game launch arguments do not have a single source")
    if common.count("project_use_full_xcode()") != 1:
        raise SystemExit("shared Xcode toolchain selection is missing")
    for relative, script in (
        ("build.sh", build),
        ("tools/build_ios.sh", ios_build),
        ("tools/package_ios.sh", package),
        ("tools/deploy_ios.sh", deploy),
    ):
        if script.count("project_use_full_xcode") != 1:
            raise SystemExit(f"{relative} does not select the full Xcode toolchain")
    if "xcrun --sdk macosx --find clang" not in build or \
       "xcrun --sdk macosx --show-sdk-path" not in build:
        raise SystemExit("macOS build does not use the Xcode SDK and clang")
    for token in ('sdk="iphoneos"', 'sdk="iphonesimulator"',
                  'xcrun --sdk "$sdk" --find clang',
                  'xcrun --sdk "$sdk" --show-sdk-path'):
        if token not in ios_build:
            raise SystemExit("iOS build does not support both Xcode SDKs: " + token)
    if deploy.count('"$PROJECT_ROOT/build.sh"') != 1 or \
       deploy.find('"$PROJECT_ROOT/build.sh"') > deploy.find("tools/build_ios.sh"):
        raise SystemExit("iOS deployment does not build translated blocks automatically")
    if "device uninstall app" in deploy or "preserving application data" not in deploy:
        raise SystemExit("iOS deployment does not preserve the existing application container")
    for token in ("device process launch", "device copy from", "MR_DIAGNOSTICS",
                  "verify_ios_startup.py", "MR_STARTUP_TIMEOUT"):
        if token not in deploy:
            raise SystemExit("iOS deployment does not verify startup")
    if common.count("project_bundle_assets()") != 1 or \
       package.count('project_bundle_assets "$APP"') != 1:
        raise SystemExit("iOS asset packaging does not use the shared implementation")
    if common.count("project_prepare_ios_app_icon()") != 1 or \
       package.count("project_prepare_ios_app_icon") != 1 or \
       deploy.count("project_prepare_ios_app_icon") != 1 or \
       package.count("project_remove_ios_app_icon") < 2 or \
       deploy.count("project_remove_ios_app_icon") < 2 or \
       package.count("project_verify_ios_app_icon") != 1:
        raise SystemExit("iOS application icon is not staged and cleaned consistently")
    if "debug)" not in run or "./run.sh debug" not in readme:
        raise SystemExit("debug run is not the single documented run.sh mode")
    for command in ("./build.sh", "./run.sh", "./test.sh", "./clean.sh"):
        if command not in readme:
            raise SystemExit(f"undocumented command: {command}")


def check_ios_project(tree: dict[str, str]) -> None:
    project = tree["ios/MinionRush.xcodeproj/project.pbxproj"]
    scheme = tree["ios/MinionRush.xcodeproj/xcshareddata/xcschemes/MinionRush.xcscheme"]
    app = tree["ios/MinionRush/MRAppDelegate.m"]
    ui_test = tree["ios/MinionRushUITests/MinionRushUITests.m"]
    simulator_test = tree["tools/test_ios_simulator.sh"]
    window = tree["src/native/window_ios.m"]
    required = (
        "Deployment.xcconfig",
        'SUPPORTED_PLATFORMS = "iphoneos iphonesimulator";',
        "SDKROOT = iphoneos;",
        "CODE_SIGN_STYLE = Automatic;",
        "APP_BUNDLE_IDENTIFIER = org.example.minionrush181;",
        'PRODUCT_BUNDLE_IDENTIFIER = "$(APP_BUNDLE_IDENTIFIER)";',
        "GENERATE_INFOPLIST_FILE = NO;",
        "INFOPLIST_FILE = Info.plist;",
        "MARKETING_VERSION = 1.8.1;",
        "QuartzCore",
        "-lminionrush",
        '"LIBRARY_SEARCH_PATHS[sdk=iphoneos*]"',
        '"LIBRARY_SEARCH_PATHS[sdk=iphonesimulator*]"',
        '"EXCLUDED_ARCHS[sdk=iphonesimulator*]" = x86_64;',
    )
    missing = [token for token in required if token not in project]
    if missing:
        raise SystemExit("incomplete iOS project setting: " + ", ".join(missing))
    if "DEVELOPMENT_TEAM = " in project:
        raise SystemExit("personal Apple development team remains in the project")
    common = tree["tools/project_common.sh"]
    deployment = tree["ios/Deployment.xcconfig"]
    if "IPHONEOS_DEPLOYMENT_TARGET = 17.0" not in deployment or \
       "IPHONEOS_DEPLOYMENT_TARGET =" in project or \
       project.count("baseConfigurationReference =") != 2 or \
       "ios/Deployment.xcconfig" not in common:
        raise SystemExit("iOS deployment target must come from the shared configuration")
    if "-Werror=unguarded-availability" not in deployment or \
       "-Werror=unguarded-availability" not in tree["tools/build_ios.sh"]:
        raise SystemExit("iOS API availability must be enforced in every build")
    for token in ("MR_DEVELOPMENT_TEAM", "MR_BUNDLE_ID", "APP_BUNDLE_IDENTIFIER",
                  "IOS_SIGNING_ARGS"):
        if token not in common:
            raise SystemExit("iOS signing is not portable: " + token)
    stale = [
        token for token in ("SWIFT_VERSION", "macosx", "xros")
        if token in project
    ]
    if "UIRequiresFullScreen" in project:
        stale.append("UIRequiresFullScreen")
    if stale:
        raise SystemExit(
            "unused platform setting in iOS project: "
            + ", ".join(stale)
        )
    info_path = ROOT / "ios/Info.plist"
    try:
        info = plistlib.loads(info_path.read_bytes())
    except (OSError, plistlib.InvalidFileException) as exc:
        raise SystemExit("invalid iOS Info.plist") from exc
    if info.get("MRGuestVersion") != "1.8.1g":
        raise SystemExit("iOS bundle lacks the 1.8.1g guest version")
    if info.get("CFBundleShortVersionString") != "$(MARKETING_VERSION)":
        raise SystemExit("iOS app version does not use the standard Xcode value")
    phone_orientations = [
        "UIInterfaceOrientationPortrait",
        "UIInterfaceOrientationLandscapeLeft",
        "UIInterfaceOrientationLandscapeRight",
    ]
    tablet_orientations = [
        "UIInterfaceOrientationPortrait",
        "UIInterfaceOrientationPortraitUpsideDown",
        "UIInterfaceOrientationLandscapeLeft",
        "UIInterfaceOrientationLandscapeRight",
    ]
    if info.get("UIRequiresFullScreen") is not True or \
       info.get("UIRequiresFullScreenIgnoredStartingWithVersion") != "26.0":
        raise SystemExit("iPad full-screen policy must preserve older orientation masks and use the versioned opt-out")
    if info.get("UISupportedInterfaceOrientations") != phone_orientations or \
       info.get("UISupportedInterfaceOrientations~ipad") != tablet_orientations:
        raise SystemExit("iOS application-level orientation declarations are incomplete")
    if not info.get("NSMotionUsageDescription"):
        raise SystemExit("iOS motion access has no usage description")
    if "hu" not in info.get("CFBundleLocalizations", []):
        raise SystemExit("iOS bundle does not advertise the Hungarian localization")
    scene_manifest = info.get("UIApplicationSceneManifest", {})
    scene_configurations = scene_manifest.get("UISceneConfigurations", {})
    application_scenes = scene_configurations.get("UIWindowSceneSessionRoleApplication", [])
    expected_scene = {
        "UISceneConfigurationName": "Minion Rush",
        "UISceneDelegateClassName": "MRSceneDelegate",
    }
    if scene_manifest.get("UIApplicationSupportsMultipleScenes") is not False or \
       expected_scene not in application_scenes:
        raise SystemExit("iOS application scene configuration is incomplete")
    if scheme.count('BlueprintIdentifier = "2613B7DE3020B2CD00727575"') != 3 or \
       scheme.count('BlueprintIdentifier = "A10000000000000000000006"') != 2 or \
       scheme.count('ReferencedContainer = "container:MinionRush.xcodeproj"') != 5:
        raise SystemExit("shared Xcode scheme does not target the application and UI tests")
    for token in ("testIntroUsesLandscapeGeometry", "testGameplayOrientationPolicy",
                  "XCUIScreen.mainScreen.screenshot"):
        if token not in ui_test:
            raise SystemExit("movie-orientation UI test is incomplete: " + token)
    for token in ("build-for-testing", "test-without-building", "MinionRushUITests"):
        if token not in simulator_test:
            raise SystemExit("Simulator UI-test workflow is incomplete: " + token)
    if "NSApplicationSupportDirectory" not in app or "NSCachesDirectory" not in app:
        raise SystemExit("iOS writable data does not use standard directories")
    if "pthread_t thread = NULL;" not in app:
        raise SystemExit("iOS engine-thread identifier is not initialized")
    if "preferredFrameRateRange" not in window or "viewDidLayoutSubviews" not in window:
        raise SystemExit("iOS presentation does not follow display and window size")
    orientation_contract = (
        "CGAffineTransformMakeRotation",
        "UIInterfaceOrientationMaskLandscape",
        "UIInterfaceOrientationMaskPortrait",
        "UIInterfaceOrientationMaskPortraitUpsideDown",
        "preferredInterfaceOrientationForPresentation",
        "setNeedsUpdateOfSupportedInterfaceOrientations",
        "setNeedsUpdateOfPrefersInterfaceOrientationLocked",
        "requestGeometryUpdateWithPreferences",
        "mr_ios_scene_geometry_changed",
        "UIDeviceOrientationDidChangeNotification",
        "beginGeneratingDeviceOrientationNotifications",
        "endGeneratingDeviceOrientationNotifications",
    )
    if any(token not in window for token in orientation_contract):
        raise SystemExit("iPhone and iPad orientation contract is incomplete")
    if "supportedInterfaceOrientationsForWindowScene" not in app or \
       "mr_ios_supported_orientations" not in app:
        raise SystemExit("iOS 27 scene orientation policy is incomplete")
    if "didUpdateCoordinateSpace" not in app or "didUpdateEffectiveGeometry" not in app or \
       "@available(iOS 26.0, *)" not in app or \
       "@available(iOS 26.0, *)" not in window or \
       "refresh_orientation_policy" not in window:
        raise SystemExit("iOS 17 and later must share an availability-aware orientation policy")
    audio_session = tree["src/native/audio_session_ios.m"]
    for token in ("@available(iOS 27.0, *)", "AVAudioSessionDidBecomeInactiveNotification",
                  "AVAudioSessionResumptionRecommendationNotification",
                  "AVAudioSessionInterruptionOptionShouldResume", "update_resumption"):
        if token not in audio_session:
            raise SystemExit("iOS 17 and later audio resumption policy is incomplete: " + token)

    privacy_path = ROOT / "ios/MinionRush/PrivacyInfo.xcprivacy"
    try:
        privacy = plistlib.loads(privacy_path.read_bytes())
    except (OSError, plistlib.InvalidFileException) as exc:
        raise SystemExit("invalid iOS privacy manifest") from exc
    declarations = {
        item.get("NSPrivacyAccessedAPIType"): set(
            item.get("NSPrivacyAccessedAPITypeReasons", [])
        )
        for item in privacy.get("NSPrivacyAccessedAPITypes", [])
    }
    expected = {
        "NSPrivacyAccessedAPICategoryFileTimestamp": {"C617.1"},
        "NSPrivacyAccessedAPICategorySystemBootTime": {"35F9.1"},
    }
    if privacy.get("NSPrivacyTracking") is not False or declarations != expected or \
       privacy.get("NSPrivacyCollectedDataTypes") != [] or \
       privacy.get("NSPrivacyTrackingDomains") != []:
        raise SystemExit("incomplete or inaccurate iOS privacy manifest")


def check_documented_environment(tree: dict[str, str]) -> None:
    """Every public MR_* switch has to be documented."""
    used: set[str] = set()
    for relative, source in tree.items():
        if relative.endswith((".c", ".m")):
            used.update(re.findall(r'getenv\("(MR_[A-Z_]+)"\)', source))
        elif relative.endswith(".sh"):
            used.update(re.findall(r"\bMR_[A-Z_]+\b", source))
    used.difference_update({"MR_BUILD_LOCK_HELD", "MR_ELF_BUILD_TOOLS",
                            "MR_UI_TEST_LANGUAGE_MENU"})
    undocumented = sorted(name for name in used if name not in tree["README.md"])
    if undocumented:
        raise SystemExit("undocumented environment variable: " + ", ".join(undocumented))


def check_offline_event(tree: dict[str, str]) -> None:
    event = tree["src/native/offline_events.c"]
    bindings = tree["src/native/game_symbols.def"]
    version_binding = 'MR_GAME_SYMBOL(EVENT_VERSION, "_ZN9EventsMgr14k_eventVersionE")'
    if bindings.count(version_binding) != 1:
        raise SystemExit("exactly one build-time event-version binding is required")
    if event.count("MR_GAME_EVENT_VERSION") != 1:
        raise SystemExit("event runtime must use the generated engine version exactly once")

    root_version = r'\"_version\":\"%s\",'
    tournament = r'\"tournament\":{'
    if event.count(root_version) != 1:
        raise SystemExit("offline event requires exactly one root-level _version field")
    if event.find(root_version) > event.find(tournament):
        raise SystemExit("offline _version is incorrectly nested in the tournament object")


def main() -> None:
    tree = read_tree()

    check_tree_shape(tree)
    check_python_syntax(tree)
    check_text_style(tree)
    check_public_repository(tree)
    check_graphics_config()
    check_no_duplication(tree)
    check_single_solutions(tree)
    check_build_time_bindings(tree)
    check_architecture(tree)
    check_platform_boundary(tree)
    check_command_separation(tree)
    check_ios_project(tree)
    check_documented_environment(tree)
    check_offline_event(tree)

    print(f"source validation: passed ({len(tree)} files, one graphics configuration, one offline system)")


if __name__ == "__main__":
    main()
