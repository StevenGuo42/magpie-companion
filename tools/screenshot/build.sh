#!/usr/bin/env bash
# Renders the UI on this computer and writes docs/screens/<page>.png.
# Needs `pio run` once (for LVGL in .pio/libdeps) and a C/C++ compiler:
#   CC=gcc CXX=g++ tools/screenshot/build.sh
# Default: zig from the PlatformIO venv (pip install ziglang).
set -euo pipefail
cd "$(dirname "$0")/../.."

PY=${PY:-.pio-venv/Scripts/python}
CC=${CC:-"$PY -m ziglang cc"}
CXX=${CXX:-"$PY -m ziglang c++"}
LVGL=.pio/libdeps/esp32-s3-touch-amoled-216/lvgl
OUT=${OUT:-.pio/screenshot}
FLAGS="-O1 -w -I tools/screenshot -I . -I $LVGL -I src -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE"

mkdir -p "$OUT/obj" docs/screens
# One at a time: parallel zig compiles stalled on its shared cache (Windows).
find "$LVGL/src" -name '*.c' | while read -r file; do
  object="$OUT/obj/$(basename "$(dirname "$file")")_$(basename "$file" .c).o"
  # shellcheck disable=SC2086
  [ -f "$object" ] || $CC $FLAGS -c "$file" -o "$object"
done
# shellcheck disable=SC2086
$CC $FLAGS -c src/magpie_logo.c -o "$OUT/logo.o"
# shellcheck disable=SC2086
$CXX $FLAGS -std=c++17 -DARDUINO -D_POSIX_THREAD_SAFE_FUNCTIONS=200112L tools/screenshot/main.cpp src/ui.cpp \
  src/app_state.cpp "$OUT/logo.o" "$OUT"/obj/*.o -o "$OUT/screenshot.exe"

"$OUT/screenshot.exe" "$OUT"
"$PY" -I tools/screenshot/to_png.py "$OUT" docs/screens
