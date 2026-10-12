#!/bin/bash
# Build an UNSIGNED native ARM64 iPhone template with an empty code section.
# Original game data and translated machine code are never included.
set -Eeuo pipefail
source "$(dirname "$0")/project_common.sh"

if (( $# > 1 )) || [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  printf 'Usage: ./tools/build_ios_template.sh [output-template.zip]\n'
  exit 2
fi

project_use_full_xcode
project_require_command python3
project_require_command xcrun
project_require_command xcodebuild
[[ ! -L "$OUT" ]] || {
  printf 'ERROR: build directory is a symlink.\n' >&2
  exit 1
}
[[ -s "$OUT/minion_code.bin" && -s "$OUT/minion_map.inc" && -s "$OUT/game_bindings.h" ]] || {
  printf 'ERROR: run ./build.sh with your own original game ZIP first.\n' >&2
  exit 1
}

output="${1:-$PROJECT_ROOT/build/native-template/MinionRush-native-template-unsigned.zip}"
[[ "$output" == /* && "$output" == *.zip ]] || {
  printf 'ERROR: output must be an absolute .zip path.\n' >&2
  exit 2
}
[[ "$output" != "$PROJECT_ROOT"/* || "$output" == "$PROJECT_ROOT/build/"* ]] || {
  printf 'ERROR: template output must not enter the public repository source tree.\n' >&2
  exit 2
}
[[ ! -e "$output" && ! -L "$output" ]] || {
  printf 'ERROR: output already exists: %s\n' "$output" >&2
  exit 1
}
lock="${TMPDIR:-/tmp}/minion-rush-build.lock"
mkdir "$lock" 2>/dev/null || {
  printf 'ERROR: another Minion Rush build is in progress.\n' >&2
  exit 1
}
cleanup() {
  rmdir "$lock" 2>/dev/null || true
}
trap cleanup EXIT

printf '== unsigned native template library ==\n'
"$PROJECT_ROOT/tools/build_ios.sh" --template

printf '== unsigned iOS application, no game assets ==\n'
derived="$PROJECT_ROOT/build/ios-native-template-noicon-derived"
bundle="$derived/Build/Products/Release-iphoneos/$IOS_SCHEME.app"
if [[ -L "$bundle" ]]; then
  printf 'ERROR: the previous template bundle is a symbolic link.\n' >&2
  exit 1
fi
if [[ -d "$bundle" ]]; then
  rm -rf -- "$bundle"
fi
xcrun xcodebuild -project "$IOS_PROJECT" -scheme "$IOS_SCHEME" \
  -configuration Release -destination 'generic/platform=iOS' \
  -derivedDataPath "$derived" \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO \
  MR_IOS_LIB_DIR=ios-template \
  ASSETCATALOG_COMPILER_APPICON_NAME= \
  APP_BUNDLE_IDENTIFIER="$IOS_BUNDLE_ID" \
  build

app="$derived/Build/Products/Release-iphoneos/$IOS_SCHEME.app"
[[ -f "$app/$IOS_SCHEME" ]] || {
  printf 'ERROR: unsigned iOS application was not produced.\n' >&2
  exit 1
}

printf '== validate and package native template ==\n'
mkdir -p "$(dirname "$output")"
python3 tools/native_ipa.py create-template \
  --app "$app" --compiled "$OUT" --output "$output"
printf 'DONE: unsigned asset-free native template: %s\n' "$output"
printf 'Do not publish this template without a copyright and signing review.\n'
