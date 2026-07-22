#!/usr/bin/env bash
S=/home/xephy/dev/qs-wallpaperengine
export HYPRLAND_INSTANCE_SIGNATURE=36b2e0cfe0c6094dbc47bd42a437431315bb3087_1784698003_1786281539
export LD_LIBRARY_PATH="$S/build/linux-wallpaperengine/build/output:/opt/linux-wallpaperengine/lib:/opt/linux-wallpaperengine"
pkill -9 -f "build2/src/quickshell" 2>/dev/null
sleep 1
# Float + fix the test window so grim can grab exactly it (no tiling overlap).
hyprctl keyword windowrulev2 "float,title:^(WE_TEST_WINDOW)$" >/dev/null 2>&1
hyprctl keyword windowrulev2 "size 640 400,title:^(WE_TEST_WINDOW)$" >/dev/null 2>&1
hyprctl keyword windowrulev2 "move 100 100,title:^(WE_TEST_WINDOW)$" >/dev/null 2>&1
"$S/build/quickshell/build2/src/quickshell" -p "${2:-$S/test/one.qml}" >/tmp/we_fs.txt 2>&1 &
QS=$!
sleep 7
grim -g "100,100 640x400" "${1:-/tmp/we_float.png}" && echo "shot -> ${1:-/tmp/we_float.png}"
kill -9 "$QS" 2>/dev/null
pkill -9 -f "build2/src/quickshell" 2>/dev/null
