#!/bin/bash
# Remove generated binaries and diagnostic logs only.
set -Eeuo pipefail
source "$(dirname "$0")/tools/project_common.sh"

if (( $# != 0 )); then
  printf 'Usage: ./clean.sh\n' >&2
  exit 2
fi

"${PYTHON:-python3}" tools/localize.py --assets-root "$ASSETS_ROOT" clean
rm -rf "$OUT" logs
project_remove_ios_app_icon
find tests tools -type d -name __pycache__ -prune -exec rm -rf {} +
find ios -type d -name xcuserdata -prune -exec rm -rf {} +
find . -maxdepth 2 -type f -name .DS_Store -delete
printf 'DONE: generated outputs and local support files were removed.\n'
