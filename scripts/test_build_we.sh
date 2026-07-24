#!/usr/bin/env bash
# test_build_we.sh — static checks only (a real compile is exercised by CI).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
s="$here/build-we.sh"
[[ -f "$s" ]]            || { echo "FAIL: build-we.sh missing"; exit 1; }
bash -n "$s"            || { echo "FAIL: syntax"; exit 1; }
grep -q 'build2' "$s"   || { echo "FAIL: must configure the build2 dir"; exit 1; }
grep -q 'WALLPAPERENGINE_SRC' "$s" || { echo "FAIL: must pass WALLPAPERENGINE_SRC"; exit 1; }
# Must be dispatchable with --print-paths without building.
out="$(REPO_ROOT=/nonexistent bash "$s" --print-paths 2>/dev/null)" || true
grep -q 'QS_BIN=' <<<"$out" || { echo "FAIL: --print-paths must emit QS_BIN="; exit 1; }
echo "PASS"
