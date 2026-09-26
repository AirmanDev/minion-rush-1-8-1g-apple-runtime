#!/bin/bash
# Shared paths, source manifests, and preflight checks.

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  printf 'This internal helper must be sourced by build.sh, run.sh, test.sh, or clean.sh.\n' >&2
  exit 2
fi

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

export LC_ALL=C
export PYTHONDONTWRITEBYTECODE=1

SRC="src/native"
OUT="build"
ASSETS_ROOT="${MR_ASSETS_ROOT:-assets}"
ENGINE="$ASSETS_ROOT/lib/libdespicablemefree.so"
GAME_FILES="$ASSETS_ROOT/game/files"
LOCALIZATION_PACKS="$ASSETS_ROOT/localizations"
APP_ICON_SOURCE="$ASSETS_ROOT/app-icon.png"
APP_ICON_PREPARER="$PROJECT_ROOT/tools/prepare_app_icon.swift"
GAME_BINARY="$OUT/minion-rush"
GAME=("$GAME_BINARY" "$ENGINE" "$GAME_FILES")
IOS_PROJECT="$PROJECT_ROOT/ios/MinionRush.xcodeproj"
IOS_SCHEME="MinionRush"
IOS_DEPLOYMENT_TARGET="$(awk '$1 == "IPHONEOS_DEPLOYMENT_TARGET" && $2 == "=" {print $3}' \
  "$PROJECT_ROOT/ios/Deployment.xcconfig")"
[[ "$IOS_DEPLOYMENT_TARGET" =~ ^[0-9]+\.[0-9]+$ ]] || {
  printf 'ERROR: invalid iOS deployment target in ios/Deployment.xcconfig\n' >&2
  exit 1
}
IOS_BUNDLE_ID="${MR_BUNDLE_ID:-org.example.minionrush181}"
IOS_APP_ICON="$PROJECT_ROOT/ios/MinionRush/Assets.xcassets/AppIcon.appiconset/AppIcon.png"
IOS_SIGNING_ARGS=()

PORTABLE_NATIVE_SOURCES=(
  main.c elf_loader.c a64_runtime.c guest_runtime.c offline_mode.c
  offline_events.c guest_threads.c shim_libc.c shim_zlib.c jni_bridge.c
  shim_gl.c shim_audio.c localization.c language_ui.c safe_area.c
)
MACOS_NATIVE_SOURCES=(
  entry_macos.c gl_context_macos.c audio_session_macos.c window_macos.m
)
IOS_NATIVE_SOURCES=(
  gl_context_ios.m audio_session_ios.m window_ios.m
)

project_require_assets() {
  [[ -f "$APP_ICON_SOURCE" ]] || {
    printf 'ERROR: application icon not found: %s\n' "$APP_ICON_SOURCE" >&2
    exit 1
  }
  [[ -f "$ENGINE" ]] || {
    printf 'ERROR: engine not found: %s\n' "$ENGINE" >&2
    exit 1
  }
  [[ -d "$GAME_FILES" ]] || {
    printf 'ERROR: game data not found: %s\n' "$GAME_FILES" >&2
    exit 1
  }
}

project_remove_ios_app_icon() {
  rm -f "$IOS_APP_ICON"
}

project_prepare_ios_app_icon() {
  local temporary
  project_require_assets
  project_require_command xcrun
  [[ -f "$APP_ICON_PREPARER" ]] || {
    printf 'ERROR: application icon preparer not found: %s\n' "$APP_ICON_PREPARER" >&2
    exit 1
  }
  project_remove_ios_app_icon
  temporary="$(mktemp -d "${TMPDIR:-/tmp}/minion-rush-app-icon.XXXXXX")"
  if ! xcrun swift -module-cache-path "$temporary/modules" \
      "$APP_ICON_PREPARER" "$APP_ICON_SOURCE" "$IOS_APP_ICON"; then
    rm -rf "$temporary"
    project_remove_ios_app_icon
    return 1
  fi
  rm -rf "$temporary"
}

project_verify_ios_app_icon() {
  local app="$1"
  local icon_name
  [[ -s "$app/Assets.car" ]] || {
    printf 'ERROR: compiled application icon catalog is missing\n' >&2
    exit 1
  }
  project_require_command plutil
  icon_name="$(plutil -extract CFBundleIcons.CFBundlePrimaryIcon.CFBundleIconName \
    raw -o - "$app/Info.plist" 2>/dev/null || true)"
  [[ "$icon_name" == "AppIcon" ]] || {
    printf 'ERROR: compiled application icon metadata is missing\n' >&2
    exit 1
  }
}

project_require_command() {
  command -v "$1" >/dev/null 2>&1 || {
    printf 'ERROR: required command not found: %s\n' "$1" >&2
    exit 1
  }
}

project_run_logged() {
  local log="$1"
  shift
  "$@" 2>&1 | tee "$log"
}

project_bundle_assets() {
  local app="$1"
  [[ -d "$app" && "$app" == *.app ]] || {
    printf 'ERROR: invalid application-bundle destination: %s\n' "$app" >&2
    exit 1
  }
  project_require_assets
  project_require_command rsync
  local sources=("$ASSETS_ROOT/lib" "$ASSETS_ROOT/game")
  [[ -d "$LOCALIZATION_PACKS" ]] && sources+=("$LOCALIZATION_PACKS")
  rsync -a --delete "${sources[@]}" "$app/"
}

project_prepare_localizations() {
  local python="${PYTHON:-python3}"
  project_require_command "$python"
  "$python" tools/localize.py --assets-root "$ASSETS_ROOT" prepare \
    --header "$OUT/localization_config.h"
}

project_use_full_xcode() {
  local developer="${DEVELOPER_DIR:-}"
  if [[ -z "$developer" ]]; then
    developer="$(xcode-select -p 2>/dev/null || true)"
    if [[ "$developer" == */CommandLineTools && \
          -d /Applications/Xcode.app/Contents/Developer ]]; then
      developer="/Applications/Xcode.app/Contents/Developer"
    fi
  fi
  [[ -d "$developer/Platforms/iPhoneOS.platform" ]] || {
    printf 'ERROR: the complete Xcode developer directory was not found.\n' >&2
    printf 'Set DEVELOPER_DIR to the Xcode Contents/Developer directory.\n' >&2
    exit 1
  }
  export DEVELOPER_DIR="$developer"
}

project_configure_ios_signing() {
  local python="${PYTHON:-python3}"
  project_require_command "$python"
  "$python" "$PROJECT_ROOT/tools/signing.py" \
    --team "${MR_DEVELOPMENT_TEAM:-}" --bundle "$IOS_BUNDLE_ID" || return $?
  IOS_SIGNING_ARGS=("APP_BUNDLE_IDENTIFIER=$IOS_BUNDLE_ID")
  if [[ -n "${MR_DEVELOPMENT_TEAM:-}" ]]; then
    IOS_SIGNING_ARGS+=("DEVELOPMENT_TEAM=$MR_DEVELOPMENT_TEAM")
  fi
}

project_validate_positive_int() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]] || {
    printf 'ERROR: expected a positive integer: %s\n' "$1" >&2
    exit 2
  }
}

project_require_current_build() {
  local stale=0
  project_require_assets
  [[ -x "$GAME_BINARY" ]] || {
    printf 'ERROR: the game is not built. Run: ./build.sh\n' >&2
    exit 1
  }
  [[ -f "$OUT/game_bindings.h" && -f "$OUT/graphics_config.h" && \
        -f "$OUT/localization_config.h" ]] || {
    printf 'ERROR: the build is incomplete. Run: ./build.sh\n' >&2
    exit 1
  }

  if [[ "$ENGINE" -nt "$GAME_BINARY" || \
        "$GAME_FILES/profiles.json" -nt "$GAME_BINARY" ]] || \
     find build.sh "$SRC" tools config resources \
       -type f -newer "$GAME_BINARY" -print -quit | grep -q .; then
    stale=1
  fi
  if [[ -d localizations ]] && \
     find localizations -type f -newer "$GAME_BINARY" -print -quit | grep -q .; then
    stale=1
  fi
  if (( stale )); then
    printf 'ERROR: the build is stale. Run: ./build.sh\n' >&2
    exit 1
  fi
}
