#!/usr/bin/env bash
# Lock the session, screenshot, unlock. Crops the widget area.
export HYPRLAND_INSTANCE_SIGNATURE=36b2e0cfe0c6094dbc47bd42a437431315bb3087_1784698003_1786281539
OUT="${1:-/tmp/we_lock.png}"
loginctl lock-session
sleep 3
grim "$OUT"
python3 -c "from PIL import Image; Image.open('$OUT').crop((1900,850,3300,1400)).save('${OUT%.png}_crop.png')" 2>/dev/null
loginctl unlock-session
sleep 1
echo "shot -> $OUT (crop ${OUT%.png}_crop.png)"
