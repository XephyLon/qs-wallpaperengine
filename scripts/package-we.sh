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

# Make the binary find its own bundled libraries.
#
# The build bakes an absolute RUNPATH pointing at the builder's own directory
# (in CI, /__w/qs-wallpaperengine/...). That path exists nowhere else, so the
# shipped binary could not resolve the libraries sitting right next to it, and
# every consumer had to work around it - immaterial-impulse rewrote the RUNPATH
# with patchelf, and where patchelf was missing it exported LD_LIBRARY_PATH
# instead. That export is inherited by every process the shell spawns, so CEF's
# bundled libEGL/libGLESv2 shadowed the system ones for every application
# launched from it.
#
# $ORIGIN is resolved by the loader at run time against the directory holding
# the binary, so bin/quickshell finds ../lib wherever the tarball is unpacked.
# Setting it here rather than in the build covers the artifact regardless of how
# quickshell itself was configured, and the staged copy is not running, so this
# cannot hit the ETXTBSY that makes an in-place repair fail on an installed one.
patchelf --set-rpath '$ORIGIN/../lib' "$stage/bin/quickshell"

# The bundled libraries need the same treatment, and fixing only the binary is
# not enough: DT_RUNPATH is NOT transitive. The executable's RUNPATH resolves
# the executable's own direct dependencies and nothing beyond them, so
# liblinux-wallpaperengine-lib.so looks up libcef.so - sitting right beside it -
# through *its own* RUNPATH, which is another builder path. Repairing just the
# binary downstream left the shell still unable to start unaided, and the
# LD_LIBRARY_PATH fallback (which IS transitive, hence why it papered over this)
# stayed in place.
#
# $ORIGIN so a library finds its siblings, then /opt/linux-wallpaperengine for
# what is not bundled at all (libkissfft-float.so.131 comes from the AUR runtime
# package, not from this build).
for so in "$stage"/lib/*.so*; do
  [[ -f "$so" ]] || continue
  patchelf --set-rpath '$ORIGIN:/opt/linux-wallpaperengine/lib:/opt/linux-wallpaperengine' "$so"
done

# Refuse to ship anything that still names a builder-local path: this is the
# whole defect, it survived every release so far, and it is invisible in the
# artifact unless something looks.
check_rpath(){ # $1=file, $2=expected
  local got; got="$(patchelf --print-rpath "$1")"
  [[ "$got" == "$2" ]] && return 0
  echo "package-we.sh: refusing to ship $(basename "$1") with RUNPATH '$got'" >&2
  return 1
}
check_rpath "$stage/bin/quickshell" '$ORIGIN/../lib' || exit 1
for so in "$stage"/lib/*.so*; do
  [[ -f "$so" ]] || continue
  check_rpath "$so" '$ORIGIN:/opt/linux-wallpaperengine/lib:/opt/linux-wallpaperengine' || exit 1
done

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
