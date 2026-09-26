#!/bin/bash
# Build and verify an ad-hoc signed, asset-free download package.
set -Eeuo pipefail
source "$(dirname "$0")/project_common.sh"
if (( $# )); then
  printf 'Usage: ./tools/package_installer.sh\n' >&2
  exit 2
fi
"$PROJECT_ROOT/tools/build_installer.sh"
"$PROJECT_ROOT/tools/test_installer.sh"
app="$PROJECT_ROOT/build/installer/Minion Rush Installer.app"
python3 tools/validate_installer.py --app "$app"
name="$(plutil -extract CFBundleName raw -o - "$app/Contents/Info.plist")"
version="$(plutil -extract CFBundleShortVersionString raw -o - "$app/Contents/Info.plist")"
output="$PROJECT_ROOT/build/releases"
mkdir -p "$output"
temporary="$(mktemp -d "$output/.release.XXXXXX")"
trap 'rm -rf -- "$temporary"' EXIT
package="$temporary/$name"
mkdir "$package"
ditto "$app" "$package/$name.app"
cp docs/INSTALLER_DOWNLOAD.md "$package/README.md"
cp LICENSE "$package/LICENSE"
archive="minion-rush-installer-$version-macos-arm64.zip"
ditto -c -k --norsrc --keepParent "$package" "$temporary/$archive"
python3 tools/validate_installer.py --app "$app" --archive "$temporary/$archive"
mkdir "$temporary/extracted"
ditto -x -k "$temporary/$archive" "$temporary/extracted"
extracted="$temporary/extracted/$name/$name.app"
python3 tools/validate_installer.py --app "$extracted"
codesign --verify --strict "$extracted"
(
  cd "$temporary"
  shasum -a 256 "$archive" > "$archive.sha256"
  shasum -a 256 -c "$archive.sha256"
)
mv -f "$temporary/$archive" "$output/$archive"
mv -f "$temporary/$archive.sha256" "$output/$archive.sha256"
printf 'DONE: %s\nChecksum: %s\n' "$output/$archive" "$output/$archive.sha256"
printf 'Ad-hoc signed, not notarized. Downloaded copies need manual Gatekeeper approval.\n'
