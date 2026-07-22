#!/usr/bin/env bash
# Run the standalone linux-wallpaperengine binary on a scene in a floating
# window, screenshot it. Confirms whether WE itself renders this scene here.
S=/home/xephy/dev/qs-wallpaperengine
BIN="${2:-$S/build/linux-wallpaperengine/build/output/linux-wallpaperengine}"
PROJ="/home/xephy/.local/share/Steam/steamapps/workshop/content/431960/3008040633"
ASSETS="$HOME/.local/share/Steam/steamapps/common/wallpaper_engine/assets"
export HYPRLAND_INSTANCE_SIGNATURE=36b2e0cfe0c6094dbc47bd42a437431315bb3087_1784698003_1786281539
export LD_LIBRARY_PATH="$S/build/linux-wallpaperengine/build/output:/opt/linux-wallpaperengine/lib:/opt/linux-wallpaperengine"
# float any linux-wallpaperengine window so grim can grab it cleanly
hyprctl keyword windowrulev2 "float,class:(linux-wallpaperengine)" >/dev/null 2>&1
hyprctl keyword windowrulev2 "size 640 360,class:(linux-wallpaperengine)" >/dev/null 2>&1
"$BIN" --window "0x0x640x360" --assets-dir "$ASSETS" "$PROJ" >/tmp/we_std.txt 2>&1 &
P=$!
sleep 6
GEO=$(hyprctl clients -j | python3 -c 'import json,sys
for c in json.load(sys.stdin):
    if "wallpaperengine" in (c.get("class","")+c.get("initialClass","")).lower():
        x,y=c["at"];w,h=c["size"];print(f"{x},{y} {w}x{h}");break')
echo "GEO=$GEO"
[ -n "$GEO" ] && grim -g "$GEO" "${1:-/tmp/we_std.png}" && echo "shot -> ${1:-/tmp/we_std.png}"
kill -9 "$P" 2>/dev/null
echo "=== WE stdout tail ==="; tail -5 /tmp/we_std.txt
