#!/bin/bash
# Run the already-built game. Use `./run.sh debug` for trace and a log ZIP.
set -Eeuo pipefail
source "$(dirname "$0")/tools/project_common.sh"

usage() {
  cat <<'USAGE'
Usage:
  ./run.sh          normal game
  ./run.sh debug    detailed trace and a log ZIP on exit
USAGE
}

run_debug() {
  project_require_command zip
  local stamp run_log trace_log archive rc
  stamp="$(date +%Y%m%d-%H%M%S)"
  mkdir -p logs
  run_log="$PROJECT_ROOT/logs/run-$stamp.log"
  trace_log="$PROJECT_ROOT/logs/trace-$stamp.log"
  archive="$PROJECT_ROOT/logs/minion-rush-logs-$stamp.zip"

  printf '== diagnostic run ==\n'
  printf 'Close the game window after enough data has been collected.\n'
  set +e
  MR_WINDOW=1 MR_DATA_BASE="$GAME_FILES" MR_LOCALIZATION_ROOT="$LOCALIZATION_PACKS" \
    MR_DIAGNOSTICS=1 MR_LOG_FRAMES=1 \
    MR_SHOW_FPS=1 MR_TRACE="$trace_log" "${run_game[@]}" 0 2>&1 | tee "$run_log"
  rc=${PIPESTATUS[0]}
  set -e

  local files=("$run_log")
  [[ -f "$trace_log" ]] && files+=("$trace_log")
  [[ -f "$OUT/rejected-candidates.tsv" ]] && \
    files+=("$OUT/rejected-candidates.tsv")
  rm -f "$archive"
  zip -j -q "$archive" "${files[@]}"
  printf 'log archive: %s\n' "$archive"
  return "$rc"
}

if (( $# > 1 )); then
  usage >&2
  exit 2
fi

mode="${1:-}"
case "$mode" in
  ""|debug) ;;
  -h|--help|help) usage; exit 0 ;;
  *)
    printf 'ERROR: unknown run mode: %s\n' "$mode" >&2
    usage >&2
    exit 2
    ;;
esac

project_require_current_build
if [[ -n "${MR_DATA_ROOT:-}" ]]; then
  data_root="$MR_DATA_ROOT"
else
  : "${HOME:?ERROR: cannot determine the macOS Application Support directory}"
  data_root="$HOME/Library/Application Support/MinionRush"
fi
mkdir -p "$data_root"
run_game=("$GAME_BINARY" "$ENGINE" "$data_root")

case "$mode" in
  "") exec env MR_WINDOW=1 MR_DATA_BASE="$GAME_FILES" \
        MR_LOCALIZATION_ROOT="$LOCALIZATION_PACKS" "${run_game[@]}" 0 ;;
  debug) run_debug ;;
esac
