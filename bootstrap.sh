#!/usr/bin/env bash
# Clone both upstreams at the commits the installed packages pin, apply this
# repo's two patch sets, and build a patched Quickshell that embeds a live
# Wallpaper Engine surface. Idempotent-ish: re-running re-copies the overlays.
#
# This does NOT touch ~/.config/quickshell. It builds into ./build/.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/build"
mkdir -p "$BUILD"

# Pinned to match the installed packages so the module ABI matches the running
# shell. Bump together with the packages.
QS_URL="https://git.outfoxxed.me/quickshell/quickshell"
QS_COMMIT="7511545ee20664e3b8b8d3322c0ffe7567c56f7a"   # illogical-impulse-quickshell-git 0.1.0.r1 (0.2.1)
WE_URL="https://github.com/Almamu/linux-wallpaperengine"
WE_COMMIT="b016d7d1"                                    # linux-wallpaperengine-git r627.b016d7d1

QS_SRC="$BUILD/quickshell"
WE_SRC="$BUILD/linux-wallpaperengine"

clone_at() { # url dir commit
	local url="$1" dir="$2" commit="$3"
	if [ ! -d "$dir/.git" ]; then git clone "$url" "$dir"; fi
	git -C "$dir" fetch --all --tags
	git -C "$dir" checkout --detach "$commit"
	git -C "$dir" submodule update --init --recursive
}

echo "==> [1/4] linux-wallpaperengine @ $WE_COMMIT"
clone_at "$WE_URL" "$WE_SRC" "$WE_COMMIT"
# Overlay the FBO driver (CFboOpenGLDriver) + its Output/Viewport.
WE_DRV="$WE_SRC/src/WallpaperEngine/Render/Drivers"
cp "$HERE/we-fbo-driver/CFboOpenGLDriver.hpp" "$WE_DRV/CFboOpenGLDriver.h" 2>/dev/null || \
cp "$HERE/we-fbo-driver/CFboOpenGLDriver.h"   "$WE_DRV/"
cp "$HERE/we-fbo-driver/CFboOpenGLDriver.cpp" "$WE_DRV/"
cp "$HERE/we-fbo-driver/CFboWindowOutput.h"   "$WE_DRV/Output/"
cp "$HERE/we-fbo-driver/CFboWindowOutput.cpp" "$WE_DRV/Output/"
cp "$HERE/we-fbo-driver/CFboOutputViewport.h" "$WE_DRV/Output/"
cp "$HERE/we-fbo-driver/CFboOutputViewport.cpp" "$WE_DRV/Output/"
cat <<'EOF'
  MANUAL (once): in linux-wallpaperengine
    - add the four CFbo*.cpp to the driver sources in src/CMakeLists.txt
    - resolve the VideoDriver ctor mouse-input arg (null MouseInput) and the
      EGL shared-context creation in CFboOpenGLDriver (see we-fbo-driver/README.md)
    - ensure the build produces liblinux-wallpaperengine-lib.so + installs the
      WallpaperEngine/Render/** headers (needed by the Quickshell module)
EOF
# cmake -S "$WE_SRC" -B "$WE_SRC/build" -DCMAKE_BUILD_TYPE=Release
# cmake --build "$WE_SRC/build" -j
export WALLPAPERENGINE_INCLUDE_DIR="$WE_SRC/src"

echo "==> [2/4] quickshell @ $QS_COMMIT"
clone_at "$QS_URL" "$QS_SRC" "$QS_COMMIT"
# Overlay the QML module.
rm -rf "$QS_SRC/src/wallpaperengine"
cp -r "$HERE/quickshell-module" "$QS_SRC/src/wallpaperengine"
cat <<'EOF'
  MANUAL (once): in quickshell (see quickshell-module/INTEGRATION.md)
    - add_subdirectory(wallpaperengine) in src/CMakeLists.txt
    - link the module into the plugin aggregate, mirroring another leaf module
    - replace the reference CMakeLists block with quickshell's qs_module macro
EOF

echo "==> [3/4] build quickshell"
# cmake -S "$QS_SRC" -B "$QS_SRC/build" -DCMAKE_BUILD_TYPE=Release \
#   -DWALLPAPERENGINE_INCLUDE_DIR="$WALLPAPERENGINE_INCLUDE_DIR"
# cmake --build "$QS_SRC/build" -j

echo "==> [4/4] package"
cat <<'EOF'
  Package as a patched illogical-impulse-quickshell-git: copy the fork PKGBUILD
    ~/.local/share/ii-vynx-fork/sdata/dist-arch/illogical-impulse-quickshell-git/
  point its source at this patched tree (or add the overlay in prepare()), then
  `makepkg -si`. Keep the pinned _commit in sync with QS_COMMIT above.
EOF

echo "Done (clone + overlay). Finish the MANUAL steps, then uncomment the cmake lines."
