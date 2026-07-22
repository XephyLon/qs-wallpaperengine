#!/usr/bin/env bash
S=/home/xephy/dev/qs-wallpaperengine
pkill -9 -f "build2/src/quickshell" 2>/dev/null
export HYPRLAND_INSTANCE_SIGNATURE=36b2e0cfe0c6094dbc47bd42a437431315bb3087_1784698003_1786281539
export LD_LIBRARY_PATH="$S/build/linux-wallpaperengine/build/output:/opt/linux-wallpaperengine/lib:/opt/linux-wallpaperengine"
OUT=/tmp/we_gdb.txt
: > "$OUT"
timeout 45 gdb -batch \
  -ex 'set pagination off' \
  -ex 'run' \
  -ex 'echo \n===CRASH BACKTRACE===\n' \
  -ex 'bt' \
  --args "$S/build/quickshell/build2/src/quickshell" -p "$S/test/one.qml" \
  >> "$OUT" 2>&1
echo "GDBEXIT=$?" >> "$OUT"
echo "written $OUT"
