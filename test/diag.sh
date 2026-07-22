#!/usr/bin/env bash
# Launch a WE test qml, wait, dump the readback diag, kill. No screenshot.
S=/home/xephy/dev/qs-wallpaperengine
export HYPRLAND_INSTANCE_SIGNATURE=36b2e0cfe0c6094dbc47bd42a437431315bb3087_1784698003_1786281539
export LD_LIBRARY_PATH="$S/build/linux-wallpaperengine/build/output:/opt/linux-wallpaperengine/lib:/opt/linux-wallpaperengine"
QML="${1:-$S/test/one.qml}"
pkill -9 -f "build2/src/quickshell" 2>/dev/null
sleep 1
rm -f /tmp/we_diag.log
"$S/build/quickshell/build2/src/quickshell" -p "$QML" >/tmp/we_diag_run.txt 2>&1 &
QS=$!
sleep 8
kill -9 "$QS" 2>/dev/null
pkill -9 -f "build2/src/quickshell" 2>/dev/null
echo "=== qml: $QML ==="
echo "=== diag ==="
cat /tmp/we_diag.log 2>/dev/null || echo "(no diag)"
echo "=== WE stderr (first frame / errors) ==="
grep -aoE "center_rgba[^\"]*|WE start failed[^\"]*|VO: \[libmpv\][^\"]*" /tmp/we_diag_run.txt 2>/dev/null | head
