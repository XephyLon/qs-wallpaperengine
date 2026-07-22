#!/usr/bin/env bash
S=/home/xephy/dev/qs-wallpaperengine
pkill -9 -f "build2/src/quickshell" 2>/dev/null
export HYPRLAND_INSTANCE_SIGNATURE=36b2e0cfe0c6094dbc47bd42a437431315bb3087_1784698003_1786281539
export LD_LIBRARY_PATH="$S/build/linux-wallpaperengine/build/output:/opt/linux-wallpaperengine/lib:/opt/linux-wallpaperengine"
DUR="${1:-12}"
timeout "$DUR" stdbuf -oL -eL "$S/build/quickshell/build2/src/quickshell" -p "$S/test/one.qml" >/tmp/we_run.txt 2>&1
echo "EXIT=$?"
