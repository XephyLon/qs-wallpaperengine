#!/usr/bin/env bash
# Run the full end4-pC shell on the patched binary for a fixed time, capture
# exit code + the WE diag log. Enables core dumps.
S=/home/xephy/dev/qs-wallpaperengine
export HYPRLAND_INSTANCE_SIGNATURE=36b2e0cfe0c6094dbc47bd42a437431315bb3087_1784698003_1786281539
export LD_LIBRARY_PATH="$S/build/linux-wallpaperengine/build/output:/opt/linux-wallpaperengine/lib:/opt/linux-wallpaperengine"
ulimit -c unlimited 2>/dev/null
DUR="${1:-15}"
pkill -9 -f "build2/src/quickshell" 2>/dev/null
pkill -x qs 2>/dev/null
sleep 1
rm -f /tmp/we_diag.log
timeout "$DUR" "$S/build/quickshell/build2/src/quickshell" -p "$HOME/.config/quickshell/end4-pC/shell.qml" >/tmp/we_shell.log 2>&1
echo "EXIT=$?"
echo "=== diag log ==="
cat /tmp/we_diag.log 2>/dev/null || echo "(no diag log written)"
