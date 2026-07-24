#!/usr/bin/env bash
# build-we.sh — build the patched Quickshell (Quickshell.WallpaperEngine module)
# + linux-wallpaperengine from a qs-wallpaperengine checkout. Single source of
# build truth: called by CI (.github/workflows/release.yml) and by the
# installer's source-build fallback (imi-unify 4.wallpaperengine.sh). Does NOT
# clone this repo — the caller provides the checkout at $REPO_ROOT.
#
# Usage:
#   bash build-we.sh              # bootstrap + compile; prints QS_BIN=/WE_LIB_DIR=
#   bash build-we.sh --print-paths  # print the paths without building
set -euo pipefail

REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
JOBS="${WE_BUILD_JOBS:-$(nproc)}"

WE_SRC="$REPO_ROOT/build/linux-wallpaperengine"
QS_SRC="$REPO_ROOT/build/quickshell"
QS_BIN="$QS_SRC/build2/src/quickshell"
WE_LIB_DIR="$WE_SRC/build/output"

print_paths(){ printf 'QS_BIN=%s\nWE_LIB_DIR=%s\n' "$QS_BIN" "$WE_LIB_DIR"; }

if [[ "${1:-}" == "--print-paths" ]]; then print_paths; exit 0; fi

# --- 1. Clone+patch both upstreams (bootstrap.sh is idempotent) -------------
cd "$REPO_ROOT"
bash ./bootstrap.sh

export WALLPAPERENGINE_SRC="$WE_SRC/src"

# --- 2. Build linux-wallpaperengine (the FBO-driver lib) --------------------
cmake -S "$WE_SRC" -B "$WE_SRC/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$WE_SRC/build" -j"$JOBS"

# --- 3. Build the patched Quickshell into build2 (Ninja; see 4.wallpaper...) -
rm -rf "$QS_SRC/build2"
cmake -GNinja -S "$QS_SRC" -B "$QS_SRC/build2" -DCMAKE_BUILD_TYPE=Release \
  -DWALLPAPERENGINE_SRC="$WE_SRC/src" -DWALLPAPERENGINE_BUILD="$WE_SRC/build" \
  -DSERVICE_MPRIS=ON -DSERVICE_NOTIFICATIONS=ON -DSERVICE_PAM=ON \
  -DSERVICE_PIPEWIRE=ON -DSERVICE_POLKIT=ON -DSERVICE_STATUS_NOTIFIER=ON \
  -DSERVICE_UPOWER=ON -DBLUETOOTH=ON
cmake --build "$QS_SRC/build2" -j"$JOBS"

[[ -x "$QS_BIN" ]] || { echo "build-we.sh: $QS_BIN missing after build" >&2; exit 1; }
print_paths
