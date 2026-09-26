#!/bin/bash
# Build, package, and sign the iOS/iPadOS app for a physical device.
set -Eeuo pipefail
source "$(dirname "$0")/project_common.sh"

CONFIG="Release"
DERIVED="${MR_IOS_DERIVED:-/tmp/minion-rush-ios}"
APP="$DERIVED/Build/Products/$CONFIG-iphoneos/$IOS_SCHEME.app"
ENTITLEMENTS="$(mktemp "${TMPDIR:-/tmp}/minion-rush-entitlements.XXXXXX")"
trap 'project_remove_ios_app_icon; rm -f "$ENTITLEMENTS"' EXIT

project_use_full_xcode
project_require_command xcodebuild
project_require_command python3
project_configure_ios_signing

if [[ -z "${MR_BUILD_LOCK_HELD:-}" ]]; then
  BUILD_LOCK="${TMPDIR:-/tmp}/minion-rush-build.lock"
  mkdir "$BUILD_LOCK" 2>/dev/null || {
    printf 'ERROR: another build is running (%s). Wait for it to finish.\n' "$BUILD_LOCK" >&2
    exit 1
  }
  trap 'project_remove_ios_app_icon; rmdir "$BUILD_LOCK" 2>/dev/null; rm -f "$ENTITLEMENTS"' EXIT
fi

project_require_command security
project_require_command codesign
project_require_assets
project_prepare_localizations
python3 tools/validate_assets.py . --assets-root "$ASSETS_ROOT"

printf '== iOS engine library ==\n'
"$PROJECT_ROOT/tools/build_ios.sh"

printf '== iOS application ==\n'
project_prepare_ios_app_icon
xcodebuild -project "$IOS_PROJECT" -scheme "$IOS_SCHEME" \
  -destination 'generic/platform=iOS' -configuration "$CONFIG" \
  -derivedDataPath "$DERIVED" -allowProvisioningUpdates \
  "${IOS_SIGNING_ARGS[@]}" \
  build
project_remove_ios_app_icon

[[ -x "$APP/$IOS_SCHEME" ]] || {
  printf 'ERROR: the Xcode build has no executable\n' >&2
  exit 1
}
project_verify_ios_app_icon "$APP"

IDENTITY="$(codesign -dvv "$APP" 2>&1 | sed -n 's/^Authority=//p' | head -1)"
[[ -n "$IDENTITY" ]] || {
  printf 'ERROR: cannot identify the Xcode application signing identity\n' >&2
  exit 1
}
codesign -d --entitlements :- "$APP" > "$ENTITLEMENTS" 2>/dev/null
[[ -s "$ENTITLEMENTS" ]] || {
  printf 'ERROR: cannot read the Xcode application entitlements\n' >&2
  exit 1
}

printf '== bundle game data ==\n'
project_bundle_assets "$APP"
printf '   %s\n' "$(du -sh "$APP" | cut -f1)"

printf '== sign packaged application ==\n'
AVAILABLE_IDENTITIES="$(security find-identity -v -p codesigning)"
[[ "$AVAILABLE_IDENTITIES" == *"$IDENTITY"* ]] || {
  printf 'ERROR: the signing identity selected by Xcode is not in the keychain: %s\n' \
    "$IDENTITY" >&2
  exit 1
}
codesign --force --sign "$IDENTITY" --timestamp=none \
  --entitlements "$ENTITLEMENTS" \
  --generate-entitlement-der "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"

printf 'DONE: %s\n' "$APP"
