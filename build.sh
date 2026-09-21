#!/bin/bash
# Clean, deterministic macOS build.
set -Eeuo pipefail
source "$(dirname "$0")/tools/project_common.sh"

if (( $# != 0 )); then
  printf 'Usage: ./build.sh\n' >&2
  exit 2
fi

project_use_full_xcode
project_require_command xcrun
CC="${CC:-$(xcrun --sdk macosx --find clang)}"
PYTHON="${PYTHON:-python3}"
MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-14.0}"
MACOS_SDK="$(xcrun --sdk macosx --show-sdk-path)"
export MACOSX_DEPLOYMENT_TARGET

COMMON_FLAGS=(
  -std=c11 -O2 -gline-tables-only
  -Wall -Wextra -Wpedantic -Werror
  -Wno-unused-parameter
  -Wno-unused-command-line-argument
  -D_DARWIN_C_SOURCE -DGL_SILENCE_DEPRECATION
  -isysroot "$MACOS_SDK"
  "-I$SRC" "-I$OUT"
)
FRAMEWORKS=(
  -framework Cocoa
  -framework QuartzCore
  -framework OpenGL
  -framework AudioToolbox
  -framework AudioUnit
)
LIBS=(-lz -lm)
LDFLAGS=(-Wl,-dead_strip -Wl,-segprot,__MRCODE,r-x,r-x)

cleanup() {
  rm -f "$OUT"/.graphics_config.h.$$
}
trap cleanup EXIT

replace_if_changed() {
  local candidate="$1" target="$2"
  if cmp -s "$candidate" "$target" 2>/dev/null; then
    rm -f "$candidate"
  else
    mv -f "$candidate" "$target"
  fi
}

verify_sources() {
  bash -n ./*.sh tools/*.sh
  "$PYTHON" tools/validate_source.py
}

generate_graphics() {
  local header_tmp="$OUT/.graphics_config.h.$$"

  printf '== unified graphics configuration ==\n'
  "$PYTHON" tools/configure_graphics.py "$GAME_FILES/profiles.json" "$header_tmp"
  replace_if_changed "$header_tmp" "$OUT/graphics_config.h"
  printf '   quality profile: fixed (profiles.json)\n'
}

build_block_compiler() {
  "$CC" "${COMMON_FLAGS[@]}" -DMR_ELF_BUILD_TOOLS \
    -o "$OUT/a64-compiler" "$SRC/a64_compiler.c" "$SRC/elf_loader.c"
}

compile_blocks() {
  printf '== ARM64 block translator ==\n'
  "$OUT/a64-compiler" "$ENGINE" "$OUT/minion_code.bin" \
    "$OUT/minion_map.inc" "$OUT/rejected-candidates.tsv" \
    "$OUT/game_bindings.h"
}

build_game() {
  local name
  local sources=()
  for name in "${PORTABLE_NATIVE_SOURCES[@]}" "${MACOS_NATIVE_SOURCES[@]}"; do
    sources+=("$SRC/$name")
  done
  printf '== macOS runtime ==\n'
  "$CC" "${COMMON_FLAGS[@]}" "${FRAMEWORKS[@]}" "${LIBS[@]}" "${LDFLAGS[@]}" \
    -o "$GAME_BINARY" \
    "${sources[@]}" "$SRC/game_code.S"
}

project_require_command "$CC"
project_require_command "$PYTHON"
project_require_assets
verify_sources
rm -rf "$OUT"
mkdir -p "$OUT"
project_prepare_localizations
generate_graphics
"$PYTHON" tools/validate_assets.py . --assets-root "$ASSETS_ROOT"
build_block_compiler
compile_blocks
build_game
printf 'DONE: %s\n' "$GAME_BINARY"
printf 'Run: ./run.sh\n'
