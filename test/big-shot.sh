#!/usr/bin/env bash
# Isolated large-resolution WE test: launch big.qml (surface fills a window that
# Hyprland tiles to full width), screenshot only that window, dump the readback.
S=/home/xephy/dev/qs-wallpaperengine
export HYPRLAND_INSTANCE_SIGNATURE=36b2e0cfe0c6094dbc47bd42a437431315bb3087_1784698003_1786281539
export LD_LIBRARY_PATH="$S/build/linux-wallpaperengine/build/output:/opt/linux-wallpaperengine/lib:/opt/linux-wallpaperengine"
pkill -9 -f "build2/src/quickshell" 2>/dev/null
sleep 1
rm -f /tmp/we_diag.log
"$S/build/quickshell/build2/src/quickshell" -p "$S/test/big.qml" >/tmp/we_big.txt 2>&1 &
QS=$!
sleep 8
GEO=$(hyprctl clients -j | python3 -c 'import json,sys
for c in json.load(sys.stdin):
    if c.get("title","")=="WE_TEST_WINDOW":
        x,y=c["at"];w,h=c["size"];print(f"{x},{y} {w}x{h}");break')
echo "GEO=$GEO"
[ -n "$GEO" ] && grim -g "$GEO" /tmp/we_bigshot.png && echo "shot -> /tmp/we_bigshot.png"
kill -9 "$QS" 2>/dev/null
pkill -9 -f "build2/src/quickshell" 2>/dev/null
echo "=== diag ==="
cat /tmp/we_diag.log 2>/dev/null || echo "(no diag)"
