#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
s="$here/package-we.sh"
[[ -f "$s" ]] || { echo "FAIL: package-we.sh missing"; exit 1; }
bash -n "$s"  || { echo "FAIL: syntax"; exit 1; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/qsbin" "$tmp/welib" "$tmp/out"
printf '#!/bin/sh\necho quickshell 0.0-test\n' > "$tmp/qsbin/quickshell"; chmod +x "$tmp/qsbin/quickshell"
printf 'x' > "$tmp/welib/liblinux-wallpaperengine-lib.so"
printf 'y' > "$tmp/welib/libcef.so"

WE_QT_VERSION=6.11.1-1 WE_COMMIT=deadbeef \
  bash "$s" --qs-bin "$tmp/qsbin/quickshell" --lib-dir "$tmp/welib" \
            --tag v0.0-test --out "$tmp/out"

tb="$tmp/out/qs-wallpaperengine-v0.0-test-x86_64.tar.zst"
[[ -f "$tb" ]]                       || { echo "FAIL: tarball not produced"; exit 1; }
[[ -f "$tmp/out/manifest.json" ]]    || { echo "FAIL: manifest missing"; exit 1; }
[[ -f "$tmp/out/SHA256SUMS" ]]       || { echo "FAIL: SHA256SUMS missing"; exit 1; }
grep -q '"qt_min": *"6.11.1-1"' "$tmp/out/manifest.json" || { echo "FAIL: qt_min not recorded"; exit 1; }
grep -q '"arch": *"x86_64"' "$tmp/out/manifest.json"     || { echo "FAIL: arch not recorded"; exit 1; }
( cd "$tmp/out" && sha256sum -c SHA256SUMS >/dev/null )  || { echo "FAIL: checksums don't verify"; exit 1; }
# tarball must contain bin/quickshell and lib/*.so
tar --use-compress-program=unzstd -tf "$tb" | grep -q '^bin/quickshell$' || { echo "FAIL: no bin/quickshell in tar"; exit 1; }
tar --use-compress-program=unzstd -tf "$tb" | grep -q '^lib/libcef.so$'  || { echo "FAIL: libs not bundled"; exit 1; }
echo "PASS"
