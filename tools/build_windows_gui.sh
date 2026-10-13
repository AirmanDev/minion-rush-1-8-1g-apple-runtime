#!/usr/bin/env bash
set -Eeuo pipefail
source "$(dirname "$0")/project_common.sh"

mode="${1:-release}"
[[ $# -le 1 && ( "$mode" == release || "$mode" == utm-test ) ]] || {
  printf 'Usage: ./tools/build_windows_gui.sh [release|utm-test]\n' >&2
  exit 2
}
python3 tools/build_builder_palette.py --check
project_require_command x86_64-w64-mingw32-gcc
mkdir -p "$OUT/windows"
target="$OUT/windows/MinionRush Builder.exe"
define=-DMR_RELEASE
if [[ "$mode" == "utm-test" ]]; then
  mkdir -p "$OUT/windows/utm-test"
  target="$OUT/windows/utm-test/MinionRush Builder.exe"
  define=-DMR_UTM_TEST
fi
x86_64-w64-mingw32-windres -i "$PROJECT_ROOT/src/windows/builder.rc" \
  -o "$OUT/windows/builder_icon.o" -O coff
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror \
  -municode -mwindows -D_WIN32_WINNT=0x0A00 "$define" \
  -Wl,--dynamicbase,--nxcompat,--high-entropy-va \
  -o "$target" "$PROJECT_ROOT/src/windows/ipa_builder.c" \
  "$OUT/windows/builder_icon.o" \
  -lole32 -luuid -lcomctl32 -lshell32 -lgdi32 -lcomdlg32
python3 - "$target" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, "tools")
from build_windows_kit import validate_gui_executable
validate_gui_executable(Path(sys.argv[1]))
print("Verified PE32+ x64 Windows GUI:", sys.argv[1])
PY

if [[ "$mode" == "release" ]]; then
  x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror \
    -municode -mwindows -D_WIN32_WINNT=0x0A00 \
    -Wl,--dynamicbase,--nxcompat,--high-entropy-va \
    -o "$OUT/windows/MinionRush Portable.exe" \
    "$PROJECT_ROOT/src/windows/onefile_launcher.c" \
    "$OUT/windows/builder_icon.o" \
    -lole32 -lshell32 -luser32 -ladvapi32
  python3 - "$OUT/windows/MinionRush Portable.exe" <<'PYLAUNCH'
import sys
from pathlib import Path
sys.path.insert(0, "tools")
from build_windows_kit import validate_gui_executable
validate_gui_executable(Path(sys.argv[1]))
print("Verified one-file launcher:", sys.argv[1])
PYLAUNCH
fi
