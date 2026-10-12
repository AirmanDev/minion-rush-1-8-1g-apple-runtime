#!/usr/bin/env bash
set -Eeuo pipefail
source "$(dirname "$0")/project_common.sh"

[[ $# -eq 0 ]] || {
  printf 'Usage: ./tools/build_windows_gui.sh\n' >&2
  exit 2
}
project_require_command x86_64-w64-mingw32-gcc
mkdir -p "$OUT/windows"
target="$OUT/windows/MinionRush Builder.exe"
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror \
  -municode -mwindows -D_WIN32_WINNT=0x0A00 \
  -Wl,--dynamicbase,--nxcompat,--high-entropy-va \
  -o "$target" "$PROJECT_ROOT/src/windows/ipa_builder.c" \
  -lole32 -luuid -lcomctl32 -lshell32 -lgdi32 -lcomdlg32
python3 - "$target" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, "tools")
from build_windows_kit import validate_gui_executable
validate_gui_executable(Path(sys.argv[1]))
print("Verified PE32+ x64 Windows GUI:", sys.argv[1])
PY
