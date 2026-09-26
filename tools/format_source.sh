#!/bin/bash
# Format or verify every maintained native and Swift source file.
set -Eeuo pipefail
source "$(dirname "$0")/project_common.sh"

usage() {
  printf 'Usage: ./tools/format_source.sh [--check|--write]\n'
}

mode="${1:---check}"
if (( $# > 1 )) || [[ "$mode" != "--check" && "$mode" != "--write" ]]; then
  usage >&2
  exit 2
fi

resolve_formatter() {
  local override="$1"
  local name="$2"
  local formatter
  if [[ -n "$override" ]]; then
    printf '%s\n' "$override"
    return
  fi
  if formatter="$(command -v "$name" 2>/dev/null)"; then
    printf '%s\n' "$formatter"
    return
  fi
  if command -v xcrun >/dev/null 2>&1 && \
      formatter="$(xcrun --find "$name" 2>/dev/null)"; then
    printf '%s\n' "$formatter"
    return
  fi
  printf 'ERROR: %s was not found\n' "$name" >&2
  return 1
}

clang_formatter="$(resolve_formatter "${CLANG_FORMAT:-}" clang-format)"
swift_formatter="$(resolve_formatter "${SWIFT_FORMAT:-}" swift-format)"
native_sources=(src/native/*.[ch] src/native/*.m ios/MinionRush/*.m)
swift_sources=(tools/*.swift installer/*.swift tests/*.swift)
if [[ "$mode" == "--write" ]]; then
  "$clang_formatter" -i "${native_sources[@]}"
  "$swift_formatter" format -i "${swift_sources[@]}"
else
  "$clang_formatter" --dry-run --Werror "${native_sources[@]}"
  "$swift_formatter" lint --strict "${swift_sources[@]}"
fi
printf 'FORMAT: passed (%s; swift-format %s)\n' \
  "$("$clang_formatter" --version)" "$("$swift_formatter" --version)"
