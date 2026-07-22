#!/usr/bin/env bash
# Launch the full patched shell detached, wait, screenshot the top-left
# background region, leave the shell running.
S=/home/xephy/dev/qs-wallpaperengine
export HYPRLAND_INSTANCE_SIGNATURE=36b2e0cfe0c6094dbc47bd42a437431315bb3087_1784698003_1786281539
export LD_LIBRARY_PATH="$S/build/linux-wallpaperengine/build/output:/opt/linux-wallpaperengine/lib:/opt/linux-wallpaperengine"
pkill -9 -f "build2/src/quickshell" 2>/dev/null
pkill -x qs 2>/dev/null
sleep 1
rm -f /tmp/we_diag.log
nohup "$S/build/quickshell/build2/src/quickshell" -p "$HOME/.config/quickshell/end4-pC/shell.qml" >/tmp/we_shell.log 2>&1 &
echo "launched $!"
sleep 8
grim -g "0,0 1600x900" "${1:-/tmp/we_live_bg.png}" && echo "shot -> ${1:-/tmp/we_live_bg.png}"
