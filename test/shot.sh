#!/usr/bin/env bash
# Launch the test shell, wait for a frame, screenshot ONLY our floating window
# (never the whole desktop), then kill.
S=/home/xephy/dev/qs-wallpaperengine
OUT="${1:-/tmp/we_shot.png}"
pkill -9 -f "build2/src/quickshell" 2>/dev/null
export HYPRLAND_INSTANCE_SIGNATURE=36b2e0cfe0c6094dbc47bd42a437431315bb3087_1784698003_1786281539
export LD_LIBRARY_PATH="$S/build/linux-wallpaperengine/build/output:/opt/linux-wallpaperengine/lib:/opt/linux-wallpaperengine"
"$S/build/quickshell/build2/src/quickshell" -p "$S/test/one.qml" >/tmp/we_run.txt 2>&1 &
QS=$!
sleep 4
# Find our window by title (FloatingWindow default title is the qml path / "quickshell").
GEO=$(hyprctl clients -j | python3 -c '
import json,sys
cs=json.load(sys.stdin)
for c in cs:
    if c.get("title","")=="WE_TEST_WINDOW":
        x,y=c["at"]; w,h=c["size"]; print(f"{x},{y} {w}x{h}"); break
')
echo "GEO=$GEO"
if [ -n "$GEO" ]; then grim -g "$GEO" "$OUT" && echo "shot -> $OUT"; else echo "window not found"; fi
kill -9 "$QS" 2>/dev/null
pkill -9 -f "build2/src/quickshell" 2>/dev/null
