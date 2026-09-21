#!/bin/bash
# Build and install on paired physical iOS/iPadOS devices.
set -Eeuo pipefail
source "$(dirname "$0")/project_common.sh"

CONFIG="Release"
DERIVED="${MR_IOS_DERIVED:-/tmp/minion-rush-ios}"
APP="$DERIVED/Build/Products/$CONFIG-iphoneos/$IOS_SCHEME.app"
LOGS="$(mktemp -d "${TMPDIR:-/tmp}/minion-rush-deploy.XXXXXX")"
trap 'rm -rf -- "$LOGS"' EXIT

project_use_full_xcode
project_require_command xcodebuild
project_require_command xcrun
project_require_command python3
project_require_command security
project_require_command plutil
project_require_assets
project_configure_ios_signing
INSTALL_TRIES="${MR_INSTALL_TRIES:-3}"
project_validate_positive_int "$INSTALL_TRIES"
STARTUP_TIMEOUT="${MR_STARTUP_TIMEOUT:-120}"
project_validate_positive_int "$STARTUP_TIMEOUT"

BUILD_LOCK="${TMPDIR:-/tmp}/minion-rush-build.lock"
mkdir "$BUILD_LOCK" 2>/dev/null || {
  printf 'ERROR: another build or deployment is running (%s). Wait for it to finish.\n' \
    "$BUILD_LOCK" >&2
  exit 1
}
cleanup() {
  local status=$?
  project_remove_ios_app_icon
  rmdir "$BUILD_LOCK" 2>/dev/null || true
  if (( status == 0 )); then
    rm -rf -- "$LOGS"
  else
    printf 'Logs: %s\n' "$LOGS" >&2
  fi
}
trap cleanup EXIT
export MR_BUILD_LOCK_HELD=1

fail_with_log() {
  printf 'ERROR: %s\n' "$1" >&2
  printf 'Last 20 lines of %s:\n' "$2" >&2
  tail -20 "$2" >&2
  exit 1
}

printf '== devices ==\n'
xcrun devicectl list devices --json-output "$LOGS/devices.json" >/dev/null 2>&1 \
  || fail_with_log 'cannot query the device list' "$LOGS/devices.json"

python3 tools/list_ios_devices.py "$LOGS/devices.json" "$@" \
  > "$LOGS/devices.txt"

[[ -s "$LOGS/devices.txt" ]] || {
  printf 'ERROR: no paired device%s\n' \
    "${1:+ matching: $*}" >&2
  exit 1
}
while IFS=$'\t' read -r udid ident name; do
  printf '   %s (%s)\n' "$name" "$udid"
done < "$LOGS/devices.txt"

printf '== translated block code ==\n'
"$PROJECT_ROOT/build.sh" > "$LOGS/build.log" 2>&1 \
  || fail_with_log 'block-code build failed' "$LOGS/build.log"

printf '== engine library ==\n'
"$PROJECT_ROOT/tools/build_ios.sh" > "$LOGS/engine.log" 2>&1 \
  || fail_with_log 'engine-library build failed' "$LOGS/engine.log"

printf '== refresh signing ==\n'
project_prepare_ios_app_icon
sign_failed=0
while IFS=$'\t' read -r udid ident name; do
  printf '   %s ... ' "$name"
  if xcodebuild -project "$IOS_PROJECT" -scheme "$IOS_SCHEME" \
       -configuration "$CONFIG" -derivedDataPath "$DERIVED" \
       -destination "platform=iOS,id=$udid" \
       -allowProvisioningUpdates -allowProvisioningDeviceRegistration \
       "${IOS_SIGNING_ARGS[@]}" \
       build > "$LOGS/sign-$udid.log" 2>&1 < /dev/null; then
    printf 'ok\n'
  else
    printf 'failed\n'
    grep -m1 -E 'error:|is not available' "$LOGS/sign-$udid.log" \
      | sed 's/^/      /' || true
    printf '      (%s)\n' "$LOGS/sign-$udid.log"
    sign_failed=1
  fi
done < "$LOGS/devices.txt"
project_remove_ios_app_icon
if (( sign_failed )); then
  printf 'ERROR: at least one device did not receive a valid signature\n' >&2
  exit 1
fi

printf '== packaging ==\n'
"$PROJECT_ROOT/tools/package_ios.sh" > "$LOGS/package.log" 2>&1 \
  || fail_with_log 'packaging failed' "$LOGS/package.log"
printf '   %s\n' "$(du -sh "$APP" | cut -f1)"
BUNDLE_ID="$(plutil -extract CFBundleIdentifier raw -o - "$APP/Info.plist")"
[[ -n "$BUNDLE_ID" ]] || {
  printf 'ERROR: cannot read the packaged bundle identifier\n' >&2
  exit 1
}

install_app() {
  local ident="$1" log="$2"
  for (( try = 1; try <= INSTALL_TRIES; try++ )); do
    if xcrun devicectl device install app --device "$ident" "$APP" \
         > "$log" 2>&1 < /dev/null; then
      return 0
    fi
    if (( try < INSTALL_TRIES )); then
      printf 'retry(%d) ' "$try"
      sleep 5
    fi
  done
  return 1
}

printf '== update installation (preserving application data) ==\n'
failed=0
while IFS=$'\t' read -r udid ident name; do
  printf '   %s ... ' "$name"
  if install_app "$ident" "$LOGS/install-$udid.log"; then
    printf 'ok\n'
  else
    printf 'ERROR after %d attempts\n' "$INSTALL_TRIES"
    tail -6 "$LOGS/install-$udid.log" | sed 's/^/      /'
    printf '      (wake and unlock the device, then run this command again)\n'
    failed=1
  fi
done < "$LOGS/devices.txt"

if (( failed )); then
  printf 'Installation failed on at least one device. Logs: %s\n' "$LOGS" >&2
  exit 1
fi

copy_app_file() {
  local ident="$1" source="$2" destination="$3" log="$4"
  rm -f -- "$destination"
  xcrun devicectl device copy from --device "$ident" \
    --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
    --source "$source" --destination "$destination" \
    > "$log" 2>&1 < /dev/null
}

printf '== startup verification ==\n'
startup_failed=0
while IFS=$'\t' read -r udid ident name; do
  printf '   %s ... ' "$name"
  if ! xcrun devicectl device process launch --device "$ident" \
       --terminate-existing --environment-variables '{"MR_DIAGNOSTICS":"1"}' \
       "$BUNDLE_ID" \
       > "$LOGS/launch-$udid.log" 2>&1 < /dev/null; then
    printf 'launch failed\n'
    tail -6 "$LOGS/launch-$udid.log" | sed 's/^/      /'
    startup_failed=1
    continue
  fi

  runtime_log="$LOGS/runtime-$udid.log"
  ready=0
  runtime_failed=0
  attempts=$(( (STARTUP_TIMEOUT + 4) / 5 ))
  for (( attempt = 1; attempt <= attempts; attempt++ )); do
    sleep 5
    if ! copy_app_file "$ident" \
         "Library/Caches/MinionRush/Logs/minion-rush.log" "$runtime_log" \
         "$LOGS/copy-log-$udid.log"; then
      continue
    fi
    if python3 tools/verify_ios_startup.py "$runtime_log"; then
      capture="$LOGS/startup-$udid.png"
      if copy_app_file "$ident" \
           "Library/Caches/MinionRush/Logs/startup.png" "$capture" \
           "$LOGS/copy-capture-$udid.log" && \
         python3 tools/verify_ios_startup.py "$runtime_log" "$capture"; then
        ready=1
      else
        printf 'invalid capture\n'
        runtime_failed=1
      fi
      break
    else
      check_status=$?
      if (( check_status == 2 )); then
        printf 'engine failure\n'
        tail -30 "$runtime_log" | sed 's/^/      /'
        runtime_failed=1
        break
      fi
    fi
  done
  if (( ready )); then
    printf 'ok\n'
  else
    if (( ! runtime_failed )) && [[ -s "$runtime_log" ]]; then
      printf 'timed out\n'
      tail -20 "$runtime_log" | sed 's/^/      /'
    fi
    startup_failed=1
  fi
done < "$LOGS/devices.txt"
if (( startup_failed )); then
  printf 'ERROR: startup was not verified on every device\n' >&2
  exit 1
fi

expiry="$(security cms -D -i "$APP/embedded.mobileprovision" 2>/dev/null \
  | plutil -extract ExpirationDate raw -o - - 2>/dev/null || true)"
[[ -n "$expiry" ]] && printf '\nprofile expires: %s\n' "$expiry"

printf 'DONE\n'
