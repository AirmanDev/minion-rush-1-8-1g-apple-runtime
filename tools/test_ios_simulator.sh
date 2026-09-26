#!/bin/bash
# Build and test the release app on clean iPhone and iPad simulators.
set -Eeuo pipefail
source "$(dirname "$0")/project_common.sh"

for major in "$@"; do
  project_validate_positive_int "$major"
done

CONFIG="Release"
DERIVED="$(mktemp -d "${TMPDIR:-/tmp}/minion-rush-simulator-build.XXXXXX")"
LOGS="$(mktemp -d "${TMPDIR:-/tmp}/minion-rush-simulator-test.XXXXXX")"
ARTIFACTS="${MR_SIMULATOR_ARTIFACTS:-}"
SIMULATOR_SETTINGS="${MR_SIMULATOR_SETTINGS:-${HOME:?}/Library/Application Support/MinionRush/settings}"
SIMULATOR_SAVE_FILE="${MR_SIMULATOR_SAVE_FILE:-}"
APP="$DERIVED/Build/Products/$CONFIG-iphonesimulator/$IOS_SCHEME.app"
CREATED_DEVICES=()

cleanup() {
  local status=$?
  project_remove_ios_app_icon
  if (( ${#CREATED_DEVICES[@]} )); then
    for device in "${CREATED_DEVICES[@]}"; do
      xcrun simctl shutdown "$device" >/dev/null 2>&1 || true
      xcrun simctl delete "$device" >/dev/null 2>&1 || true
    done
  fi
  rm -rf -- "$DERIVED"
  if [[ -n "$ARTIFACTS" ]]; then
    mkdir -p -- "$ARTIFACTS"
    cp -R "$LOGS"/. "$ARTIFACTS"/
    rm -rf -- "$LOGS"
    printf 'Artifacts: %s\n' "$ARTIFACTS"
  elif (( status == 0 )); then
    rm -rf -- "$LOGS"
  else
    printf 'Logs: %s\n' "$LOGS" >&2
  fi
}
trap cleanup EXIT

project_use_full_xcode
project_require_command xcodebuild
project_require_command xcrun
project_require_command python3
project_require_command plutil
xcrun simctl list runtimes --json > "$LOGS/runtimes.json"
xcrun simctl list devicetypes --json > "$LOGS/device-types.json"
python3 tools/select_ios_simulators.py "$LOGS/runtimes.json" "$LOGS/device-types.json" \
  "$IOS_DEPLOYMENT_TARGET" "$@" > "$LOGS/matrix.tsv"
project_require_assets
[[ -f "$SIMULATOR_SETTINGS" ]] || {
  printf 'ERROR: engine-generated Simulator settings file not found: %s\n' \
    "$SIMULATOR_SETTINGS" >&2
  printf 'Complete the intro once on macOS or set MR_SIMULATOR_SETTINGS.\n' >&2
  exit 1
}
if [[ -n "$SIMULATOR_SAVE_FILE" && ! -f "$SIMULATOR_SAVE_FILE" ]]; then
  printf 'ERROR: Simulator save file not found: %s\n' "$SIMULATOR_SAVE_FILE" >&2
  exit 1
fi

printf '== translated block code ==\n'
"$PROJECT_ROOT/build.sh" > "$LOGS/build.log" 2>&1

printf '== Simulator engine library ==\n'
"$PROJECT_ROOT/tools/build_ios.sh" --platform simulator > "$LOGS/engine.log" 2>&1

printf '== Simulator application ==\n'
project_prepare_ios_app_icon
xcodebuild -project "$IOS_PROJECT" -scheme "$IOS_SCHEME" \
  -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' \
  -configuration "$CONFIG" -derivedDataPath "$DERIVED" \
  CODE_SIGNING_ALLOWED=NO build-for-testing > "$LOGS/xcodebuild.log" 2>&1
project_remove_ios_app_icon
project_bundle_assets "$APP"
project_verify_ios_app_icon "$APP"
BUNDLE_ID="$(plutil -extract CFBundleIdentifier raw -o - "$APP/Info.plist")"

run_ui_test() {
  local phase="$1" label="$2" udid="$3" method="$4"
  local data status=0
  xcodebuild \
    -project "$IOS_PROJECT" -scheme "$IOS_SCHEME" -configuration "$CONFIG" \
    -derivedDataPath "$DERIVED" -destination "platform=iOS Simulator,id=$udid" \
    -resultBundlePath "$LOGS/$phase-$label.xcresult" \
    "-only-testing:MinionRushUITests/MinionRushUITests/$method" \
    test-without-building > "$LOGS/$phase-$label.log" 2>&1 || status=$?
  data="$(xcrun simctl get_app_container "$udid" "$BUNDLE_ID" data 2>/dev/null || true)"
  if [[ -n "$data" ]]; then
    cp "$data/Library/Caches/MinionRush/Logs/minion-rush.log" \
      "$LOGS/$phase-runtime-$label.log" 2>/dev/null || true
  fi
  if (( status != 0 )); then
    printf 'ERROR: %s Simulator %s UI test failed\n' "$label" "$phase" >&2
    tail -80 "$LOGS/$phase-$label.log" >&2
    return 1
  fi
}

run_test() {
  local kind="$1" device_type="$2" label="$3"
  local udid data log ui_log orientation_log capture attempt state
  local settings_dir
  local landscape_line
  udid="$(xcrun simctl create "Minion Rush test $label" "$device_type" "$runtime")"
  CREATED_DEVICES+=("$udid")
  xcrun simctl boot "$udid"
  xcrun simctl bootstatus "$udid" -b

  if [[ "${MR_SIMULATOR_SKIP_INTRO:-0}" != 1 ]]; then
    run_ui_test intro "$label" "$udid" testIntroUsesLandscapeGeometry
    log="$LOGS/intro-runtime-$label.log"
    landscape_line="$(grep -nFm1 '[Orientation] landscape surface:' "$log" \
      | cut -d: -f1 || true)"
    if [[ -z "$landscape_line" ]]; then
      printf 'ERROR: %s Simulator did not enter landscape intro rendering\n' "$label" >&2
      return 1
    fi
    if [[ "$kind" == "iPad" ]] && \
       ! grep -Eq '\[Orientation\] scene geometry changed: landscape-(left|right)' "$log"; then
      printf 'ERROR: iPad Simulator did not rotate the intro scene to landscape\n' >&2
      return 1
    fi
  fi

  xcrun simctl install "$udid" "$APP"
  data="$(xcrun simctl get_app_container "$udid" "$BUNDLE_ID" data)"
  settings_dir="$data/Library/Application Support/MinionRush"
  mkdir -p -- "$settings_dir"
  cp "$SIMULATOR_SETTINGS" "$settings_dir/settings"
  if [[ -n "$SIMULATOR_SAVE_FILE" ]]; then
    cp "$SIMULATOR_SAVE_FILE" "$settings_dir/savegame"
  fi

  run_ui_test settings "$label" "$udid" testHungarianButtonUsesEngineSettingsPage
  ui_log="$LOGS/settings-runtime-$label.log"
  grep -Fq '[Localization UI] hu added to settings at' "$ui_log" || {
    printf 'ERROR: %s Simulator did not add Hungarian to the engine settings page\n' "$label" >&2
    return 1
  }
  if [[ "$kind" == "iPhone" ]]; then
    grep -Fq '[Localization UI] selected hu from settings' "$ui_log" || {
      printf 'ERROR: %s Simulator could not select Hungarian through the engine button\n' \
        "$label" >&2
      return 1
    }
  fi

  run_ui_test orientation "$label" "$udid" testGameplayOrientationPolicy
  orientation_log="$LOGS/orientation-runtime-$label.log"
  awk '
    /^\[audio\] session reactivation requested: application became active$/ {
      requested = 1; resumed = 0
    }
    requested && /^\[audio\] session active / { resumed = 1 }
    END { exit !resumed }
  ' "$orientation_log" || {
    printf 'ERROR: %s Simulator did not complete foreground audio recovery\n' "$label" >&2
    return 1
  }
  if [[ "$kind" == "iPad" ]]; then
    grep -Fxq '[Orientation] content: portrait' "$orientation_log" || {
      printf 'ERROR: %s Simulator did not render upright portrait\n' "$label" >&2
      return 1
    }
    grep -Fq '[Orientation] content: portrait-upside-down' \
      "$orientation_log" || {
      printf 'ERROR: %s Simulator did not render upside-down portrait\n' "$label" >&2
      return 1
    }
    if grep -Eq '\[Orientation\] content: landscape-(left|right)' \
      "$orientation_log"; then
      printf 'ERROR: %s Simulator rendered gameplay in landscape\n' "$label" >&2
      return 1
    fi
  fi
  if [[ -n "$SIMULATOR_SAVE_FILE" ]]; then
    run_ui_test result "$label" "$udid" testResultScreenRemainsResponsive
  fi

  SIMCTL_CHILD_MR_LANGUAGE=hu \
  SIMCTL_CHILD_MR_DIAGNOSTICS=1 \
  SIMCTL_CHILD_MR_GUEST_LOG=1 \
  SIMCTL_CHILD_MR_STARTUP_TEST_MS=120000 \
    xcrun simctl launch --terminate-running-process "$udid" "$BUNDLE_ID"

  data="$(xcrun simctl get_app_container "$udid" "$BUNDLE_ID" data)"
  log="$data/Library/Caches/MinionRush/Logs/minion-rush.log"
  capture="$data/Library/Caches/MinionRush/Logs/startup.png"
  state=1
  for (( attempt = 1; attempt <= 120; attempt++ )); do
    if [[ -f "$log" ]]; then
      if python3 tools/verify_ios_startup.py "$log" "$capture" portrait; then
        state=0
        break
      else
        state=$?
        (( state == 2 )) && break
      fi
    fi
    sleep 1
  done
  cp "$log" "$LOGS/runtime-$label.log" 2>/dev/null || true
  cp "$capture" "$LOGS/startup-$label.png" 2>/dev/null || true
  if (( state != 0 )); then
    printf 'ERROR: %s Simulator startup failed\n' "$label" >&2
    [[ -f "$log" ]] && tail -30 "$log" >&2
    return 1
  fi
  grep -Fq 'language: hu (engine code hu)' "$log" || {
    printf 'ERROR: %s Simulator did not activate the native hu engine locale\n' "$label" >&2
    return 1
  }
  printf '   %s: Hungarian startup and portrait policy passed\n' "$label"
  xcrun simctl shutdown "$udid"
}

while IFS=$'\t' read -r family runtime phone_type tablet_type; do
  printf '== %s Simulator coverage ==\n' "$family"
  run_test "iPhone" "$phone_type" "$family-iPhone"
  run_test "iPad" "$tablet_type" "$family-iPad"
done < "$LOGS/matrix.tsv"
printf 'SIMULATOR TEST: passed\n'
