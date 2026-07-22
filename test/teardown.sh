#!/usr/bin/env bash
S=/home/xephy/dev/qs-wallpaperengine
export HYPRLAND_INSTANCE_SIGNATURE=36b2e0cfe0c6094dbc47bd42a437431315bb3087_1784698003_1786281539
export LD_LIBRARY_PATH="$S/build/linux-wallpaperengine/build/output:/opt/linux-wallpaperengine/lib:/opt/linux-wallpaperengine"
pkill -9 -f "build2/src/quickshell" 2>/dev/null
pkill -x qs 2>/dev/null
sleep 1
nohup "$S/build/quickshell/build2/src/quickshell" -p "$HOME/.config/quickshell/end4-pC/shell.qml" >/tmp/we_td.log 2>&1 &
QP=$!
sleep 7
echo "=== sending SIGTERM to $QP ==="
kill -TERM "$QP"
sleep 4
if pgrep -f "build2/src/quickshell" >/dev/null; then echo "STILL ALIVE (teardown hung)"; else echo "exited cleanly"; fi
echo "=== teardown log ==="
grep -aE "WeThread:" /tmp/we_td.log | tail -14
