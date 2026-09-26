#!/bin/bash
# Test native process handling and UI state without assets or device installation.
set -Eeuo pipefail
source "$(dirname "$0")/project_common.sh"
if (( $# )); then
  printf 'Usage: ./tools/test_installer.sh\n' >&2
  exit 2
fi
project_use_full_xcode
app="$PROJECT_ROOT/build/installer/Minion Rush Installer.app"
[[ -d "$app" ]] || {
  printf 'ERROR: build the installer with tools/build_installer.sh first.\n' >&2
  exit 1
}
temporary="$(mktemp -d "${TMPDIR:-/tmp}/minion-rush-installer-test.XXXXXX")"
trap 'rm -rf -- "$temporary"' EXIT
minimum="$(plutil -extract LSMinimumSystemVersion raw -o - installer/Info.plist)"
xcrun swiftc -swift-version 6 -strict-concurrency=complete -warnings-as-errors \
  -sdk "$(xcrun --sdk macosx --show-sdk-path)" -target "arm64-apple-macos$minimum" \
  -module-cache-path "$temporary/modules" \
  installer/BackendProcess.swift installer/InstallerModel.swift tests/InstallerTests.swift \
  -o "$temporary/installer-tests"
python3 tools/run_with_timeout.py 30 \
  "$temporary/installer-tests" "$app/Contents/Resources"
