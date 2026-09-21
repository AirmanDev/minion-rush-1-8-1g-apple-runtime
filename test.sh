#!/bin/bash
# Validate and smoke-test the current build. This script never compiles it.
set -Eeuo pipefail
source "$(dirname "$0")/tools/project_common.sh"

usage() {
  printf 'Usage: ./test.sh [--static] [headless_frames] [window_timeout_seconds]\n'
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" || "${1:-}" == "help" ]]; then
  usage
  exit 0
fi
STATIC_ONLY=0
if [[ "${1:-}" == "--static" ]]; then
  STATIC_ONLY=1
  shift
fi
if (( $# > 2 )); then
  usage >&2
  exit 2
fi

PYTHON="${PYTHON:-python3}"
HEADLESS_FRAMES="${1:-30}"
WINDOW_TIMEOUT_SECONDS="${2:-120}"

project_validate_positive_int "$HEADLESS_FRAMES"
project_validate_positive_int "$WINDOW_TIMEOUT_SECONDS"
project_require_command "$PYTHON"
bash -n ./*.sh tools/*.sh
PYTHONDONTWRITEBYTECODE=1 "$PYTHON" -m unittest discover -s tests -v
"$PYTHON" tools/validate_source.py
"$PYTHON" tools/localize.py --assets-root "$ASSETS_ROOT" validate
"$PYTHON" tools/validate_assets.py . --assets-root "$ASSETS_ROOT"

if (( STATIC_ONLY )); then
  printf 'STATIC TEST: passed\n'
  exit 0
fi

project_require_current_build

runtime_data="$(mktemp -d "${TMPDIR:-/tmp}/minion-rush-test.XXXXXX")"
cleanup_runtime() {
  rm -rf "$runtime_data"
}
trap cleanup_runtime EXIT
test_game=("$GAME_BINARY" "$ENGINE" "$runtime_data")
asset_sentinel="$runtime_data/assets-are-read-only"
touch "$asset_sentinel"

printf '== headless startup test: %s frames ==\n' "$HEADLESS_FRAMES"
MR_WINDOW=0 MR_DATA_BASE="$GAME_FILES" MR_LOCALIZATION_ROOT="$LOCALIZATION_PACKS" \
  "${test_game[@]}" "$HEADLESS_FRAMES"

printf '== windowed startup and audio test: at most %s seconds ==\n' \
  "$WINDOW_TIMEOUT_SECONDS"
printf '   The test exits after startup services and a subsequent PCM signal.\n'
set +e
MR_WINDOW=1 MR_DIAGNOSTICS=1 \
  MR_DATA_BASE="$GAME_FILES" \
  MR_LOCALIZATION_ROOT="$LOCALIZATION_PACKS" \
  MR_STARTUP_TEST_MS="$((WINDOW_TIMEOUT_SECONDS * 1000))" \
  "$PYTHON" tools/run_with_timeout.py "$((WINDOW_TIMEOUT_SECONDS + 5))" \
  "${test_game[@]}" 0
window_status=$?
set -e
if (( window_status == 124 )); then
  printf 'ERROR: the external watchdog stopped the windowed test\n' >&2
  exit 1
fi
if (( window_status != 0 )); then
  printf 'ERROR: the windowed test exited with status %d\n' "$window_status" >&2
  exit "$window_status"
fi

if find "$ASSETS_ROOT" -type f -newer "$asset_sentinel" -print -quit | grep -q .; then
  printf 'ERROR: the runtime test modified source assets\n' >&2
  exit 1
fi

printf 'TEST: passed\n'
