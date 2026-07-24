#!/usr/bin/env bash
# package-we.sh — assemble a prebuilt qs-wallpaperengine release artifact:
#   <out>/qs-wallpaperengine-<tag>-x86_64.tar.zst   (bin/quickshell + lib/*.so*)
#   <out>/manifest.json                             (version, commit, qt_min, arch, files)
#   <out>/SHA256SUMS                                (over the tarball + manifest)
set -euo pipefail

QS_BIN="" LIB_DIR="" TAG="" OUT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --qs-bin)  QS_BIN="$2"; shift 2;;
    --lib-dir) LIB_DIR="$2"; shift 2;;
    --tag)     TAG="$2"; shift 2;;
    --out)     OUT="$2"; shift 2;;
    *) echo "package-we.sh: unknown arg $1" >&2; exit 2;;
  esac
done
[[ -x "$QS_BIN" && -d "$LIB_DIR" && -n "$TAG" && -n "$OUT" ]] || {
  echo "usage: package-we.sh --qs-bin B --lib-dir D --tag T --out O" >&2; exit 2; }
[[ "$TAG" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "package-we.sh: bad tag $TAG" >&2; exit 2; }

ARCH="$(uname -m)"
QT_MIN="${WE_QT_VERSION:-$(pacman -Q qt6-base 2>/dev/null | awk '{print $2}')}"
COMMIT="${WE_COMMIT:-unknown}"
BUILT_AT="${WE_BUILT_AT:-unknown}"   # CI passes an ISO timestamp; scripts can't call date() deterministically in tests

mkdir -p "$OUT"
stage="$(mktemp -d)"; trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/bin" "$stage/lib"
install -m755 "$QS_BIN" "$stage/bin/quickshell"
# copy every regular .so* the WE build produced (libcef, libEGL, libGLESv2,
# libvk_swiftshader, liblinux-wallpaperengine-lib.so, ...)
find "$LIB_DIR" -maxdepth 1 -type f -name '*.so*' -exec cp -a {} "$stage/lib/" \;

# manifest.json (hand-built JSON; values are our own, no external interpolation)
files_json="$(cd "$stage" && find bin lib -type f | sort | sed 's/.*/"&"/' | paste -sd, -)"
cat > "$OUT/manifest.json" <<JSON
{
  "schema": 1,
  "version": "$TAG",
  "commit": "$COMMIT",
  "qt_min": "$QT_MIN",
  "arch": "$ARCH",
  "built_at": "$BUILT_AT",
  "files": [$files_json]
}
JSON

tarball="qs-wallpaperengine-${TAG}-${ARCH}.tar.zst"
tar --use-compress-program='zstd -19 -T0' -C "$stage" -cf "$OUT/$tarball" bin lib

( cd "$OUT" && sha256sum "$tarball" manifest.json > SHA256SUMS )
echo "package-we.sh: wrote $OUT/$tarball + manifest.json + SHA256SUMS"
