#!/bin/bash
# Build the iOS/iPadOS engine library used by the Xcode target.
set -Eeuo pipefail
source "$(dirname "$0")/project_common.sh"

usage() {
  printf 'Usage: ./tools/build_ios.sh [--platform device|simulator] [output_directory]\n'
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" || "${1:-}" == "help" ]]; then
  usage
  exit 0
fi

platform="device"
if [[ "${1:-}" == "--platform" ]]; then
  [[ $# -ge 2 ]] || {
    usage >&2
    exit 2
  }
  platform="$2"
  shift 2
fi
if (( $# > 1 )) || [[ "${1:-}" == -* ]]; then
  usage >&2
  exit 2
fi
case "$platform" in
  device)
    sdk="iphoneos"
    target="arm64-apple-ios${IOS_DEPLOYMENT_TARGET}"
    default_output="$OUT/ios"
    ;;
  simulator)
    sdk="iphonesimulator"
    target="arm64-apple-ios${IOS_DEPLOYMENT_TARGET}-simulator"
    default_output="$OUT/ios-simulator"
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac

project_use_full_xcode
project_require_command xcrun
CC="${CC:-$(xcrun --sdk "$sdk" --find clang)}"
SDK="$(xcrun --sdk "$sdk" --show-sdk-path)"
OUT_DIR="${1:-$default_output}"

FLAGS=(
  -std=c11 -O2 -gline-tables-only
  -Wall -Wextra -Wpedantic -Werror
  -Werror=unguarded-availability
  -Wno-unused-parameter
  -fvisibility=hidden -ffunction-sections -fdata-sections
  -D_DARWIN_C_SOURCE -DGL_SILENCE_DEPRECATION -DGLES_SILENCE_DEPRECATION
  -isysroot "$SDK" -target "$target"
  "-I$SRC" "-I$OUT"
)

require_generated_code() {
  if [[ ! -f "$OUT/minion_code.bin" || ! -f "$OUT/game_bindings.h" ]]; then
    printf 'ERROR: generated block code is missing. Run ./build.sh first.\n' >&2
    exit 1
  fi
}

project_require_command "$CC"
require_generated_code
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

printf '== iOS %s engine library (%s) ==\n' "$platform" "$target"
objects=()
for source in "${PORTABLE_NATIVE_SOURCES[@]}"; do
  name="${source%.*}"
  printf 'Compile: %s\n' "$source"
  "$CC" -c "${FLAGS[@]}" -o "$OUT_DIR/$name.o" "$SRC/$source"
  objects+=("$OUT_DIR/$name.o")
done
for source in "${IOS_NATIVE_SOURCES[@]}"; do
  name="${source%.*}"
  printf 'Compile: %s\n' "$source"
  "$CC" -c -fobjc-arc "${FLAGS[@]}" -o "$OUT_DIR/$name.o" "$SRC/$source"
  objects+=("$OUT_DIR/$name.o")
done

printf 'Compile: game_code.S\n'
"$CC" -c -std=c11 -isysroot "$SDK" -target "$target" \
  "-I$PROJECT_ROOT" -o "$OUT_DIR/game_code.o" "$SRC/game_code.S"
objects+=("$OUT_DIR/game_code.o")

xcrun libtool -static -o "$OUT_DIR/libminionrush.a" "${objects[@]}"
printf '   %s sources, %s\n' "${#objects[@]}" \
  "$(du -h "$OUT_DIR/libminionrush.a" | cut -f1) libminionrush.a"
printf 'DONE: %s\n' "$OUT_DIR/libminionrush.a"
