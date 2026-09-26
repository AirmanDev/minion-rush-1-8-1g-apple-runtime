#!/bin/bash
# Package the native installer with public sources only.
set -Eeuo pipefail
source "$(dirname "$0")/project_common.sh"

identity="-"
if (( $# )); then
  if (( $# != 2 )) || [[ "$1" != "--identity" || -z "$2" ]]; then
    printf 'Usage: ./tools/build_installer.sh [--identity "Developer ID Application: ..."]\n' >&2
    exit 2
  fi
  identity="$2"
fi
project_use_full_xcode
project_require_command python3
python3 tools/validate_source.py
minimum="$(plutil -extract LSMinimumSystemVersion raw -o - installer/Info.plist)"
version="$(xcrun xcodebuild -version)"
[[ "$version" =~ ^Xcode\ (2[7-9]|[3-9][0-9]) ]] || {
  printf 'ERROR: Xcode 27 or later is required.\n' >&2
  exit 1
}
output="$PROJECT_ROOT/build/installer"
mkdir -p "$output"
temporary="$(mktemp -d "$output/.package.XXXXXX")"
trap 'rm -rf -- "$temporary"' EXIT
app="$temporary/Minion Rush Installer.app"
resources="$app/Contents/Resources"
mkdir -p "$app/Contents/MacOS" "$resources/Runtime"
cp installer/Info.plist "$app/Contents/Info.plist"
cp config/installer_ui.json "$resources/installer_ui.json"
while IFS= read -r name; do
  mkdir -p "$resources/Runtime/$(dirname "$name")"
  cp -p "$name" "$resources/Runtime/$name"
done < config/source_manifest.txt
xcrun swift -module-cache-path "$temporary/modules" \
  tools/prepare_installer_icon.swift "$temporary/Installer.iconset"
iconutil -c icns "$temporary/Installer.iconset" -o "$resources/Installer.icns"
xcrun swiftc -O -swift-version 6 -strict-concurrency=complete -warnings-as-errors \
  -sdk "$(xcrun --sdk macosx --show-sdk-path)" -target "arm64-apple-macos$minimum" \
  -module-cache-path "$temporary/modules" \
  installer/*.swift -o "$app/Contents/MacOS/MinionRushInstaller"
signing=(--force --sign "$identity" --options runtime)
if [[ "$identity" != "-" ]]; then signing+=(--timestamp); fi
codesign "${signing[@]}" "$app"
codesign --verify --strict "$app"
destination="$output/Minion Rush Installer.app"
if [[ -e "$destination" ]]; then
  mv "$destination" "$temporary/previous.app"
fi
mv "$app" "$destination"
printf 'DONE: %s\n' "$destination"
if [[ "$identity" == "-" ]]; then
  printf 'Local ad-hoc build. Public downloads require Developer ID signing and notarization.\n'
fi
