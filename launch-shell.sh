#!/usr/bin/env bash
#
# Run the end4-pC shell with the PATCHED Quickshell binary that embeds the
# Quickshell.WallpaperEngine module. Reversible: Ctrl-C / kill it and relaunch
# your normal quickshell to go back to the stock shell.
#
# Set a Wallpaper Engine wallpaper first (wallpaper selector -> WE tab, or set
# wallpaperSelector.wallpaperEngine.activePath in the illogical-impulse config)
# so Background.qml's WE layer activates. With no active WE project it renders
# the normal static wallpaper.
set -euo pipefail

S=/home/xephy/dev/qs-wallpaperengine
BIN="$S/build/quickshell/build2/src/quickshell"
CONFIG="$HOME/.config/quickshell/end4-pC/shell.qml"

export LD_LIBRARY_PATH="$S/build/linux-wallpaperengine/build/output:/opt/linux-wallpaperengine/lib:/opt/linux-wallpaperengine${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

echo "Stopping stock shell (qs / quickshell)..."
pkill -x qs 2>/dev/null || true
pkill -x quickshell 2>/dev/null || true
sleep 0.5

echo "Launching patched shell: $BIN -p $CONFIG"
exec "$BIN" -p "$CONFIG"
