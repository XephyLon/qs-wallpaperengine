#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
s="$here/package-we.sh"
[[ -f "$s" ]] || { echo "FAIL: package-we.sh missing"; exit 1; }
bash -n "$s"  || { echo "FAIL: syntax"; exit 1; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/qsbin" "$tmp/welib" "$tmp/out"
# A real ELF, not a shell script: packaging now rewrites the binary's RUNPATH,
# and patchelf cannot operate on a #! script. Giving it an absolute RUNPATH
# first reproduces what the build actually hands over - the builder's own
# directory, which exists on no other machine (#12).
command -v patchelf >/dev/null || { echo "SKIP: patchelf not installed"; exit 0; }
# `env`, not `true`: `true` is a shell builtin, so `command -v` yields the word
# rather than a path and there is nothing to copy.
cp "$(command -v env)" "$tmp/qsbin/quickshell"; chmod +x "$tmp/qsbin/quickshell"
patchelf --set-rpath '/__w/qs-wallpaperengine/build/output' "$tmp/qsbin/quickshell"
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

# The shipped binary must locate its own bundled libraries. Absolute builder
# paths shipped in every release up to now, and nothing looked - so the check is
# on the artifact itself, unpacked, not on what the script claims to have done.
x="$tmp/x"; mkdir -p "$x"
tar --use-compress-program=unzstd -xf "$tb" -C "$x"
rp="$(patchelf --print-rpath "$x/bin/quickshell")"
[[ "$rp" == '$ORIGIN/../lib' ]] \
  || { echo "FAIL: shipped RUNPATH is '$rp', expected \$ORIGIN/../lib"; exit 1; }
case "$rp" in
  /*|*:/*) echo "FAIL: shipped RUNPATH contains an absolute builder path: $rp"; exit 1;;
esac

# And packaging must refuse rather than ship a bad one. Re-run with a patchelf
# stub that quietly declines to set the rpath: the guard has to catch it.
mkdir -p "$tmp/stub"
printf '#!/usr/bin/env bash\nif [[ "$1" == "--set-rpath" ]]; then exit 0; fi\nexec %s "$@"\n' \
  "$(command -v patchelf)" > "$tmp/stub/patchelf"
chmod +x "$tmp/stub/patchelf"
if PATH="$tmp/stub:$PATH" WE_QT_VERSION=6.11.1-1 WE_COMMIT=deadbeef \
     bash "$s" --qs-bin "$tmp/qsbin/quickshell" --lib-dir "$tmp/welib" \
               --tag v0.0-test --out "$tmp/out2" 2>/dev/null; then
  echo "FAIL: packaged a binary whose RUNPATH was never rewritten"; exit 1
fi

echo "PASS"
