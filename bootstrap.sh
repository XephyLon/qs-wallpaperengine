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
cp "$HERE/we-fbo-driver/NullMouseInput.h" "$WE_SRC/src/WallpaperEngine/Input/"

# Let a later driver registration OVERRIDE an earlier one (default uses emplace,
# which ignores duplicates, so GLFW's window driver would always win over ours).
sed -i 's/cur->second.emplace (xdgSessionType, factory);/cur->second.insert_or_assign (xdgSessionType, factory);/; s/map.emplace (xdgSessionType, factory);/map.insert_or_assign (xdgSessionType, factory);/' \
	"$WE_SRC/src/WallpaperEngine/Render/Drivers/VideoFactories.cpp"

# Register the four .cpp in the lib's COMMON_SOURCES, after the GLFW driver.
WE_CMAKE="$WE_SRC/CMakeLists.txt"
if ! grep -q 'CFboOpenGLDriver.cpp' "$WE_CMAKE"; then
	python3 - "$WE_CMAKE" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
anchor = "    src/WallpaperEngine/Render/Drivers/GLFWOpenGLDriver.cpp\n"
add = (
    "    src/WallpaperEngine/Render/Drivers/Output/CFboWindowOutput.cpp\n"
    "    src/WallpaperEngine/Render/Drivers/Output/CFboWindowOutput.h\n"
    "    src/WallpaperEngine/Render/Drivers/Output/CFboOutputViewport.cpp\n"
    "    src/WallpaperEngine/Render/Drivers/Output/CFboOutputViewport.h\n"
    "    src/WallpaperEngine/Render/Drivers/CFboOpenGLDriver.h\n"
    "    src/WallpaperEngine/Render/Drivers/CFboOpenGLDriver.cpp\n"
)
open(p, 'w').write(s.replace(anchor, anchor + add, 1))
PY
fi
cat <<'EOF'
  DONE automatically: CFbo* copied + registered in CMakeLists, NullMouseInput added.
  Remaining in CFboOpenGLDriver.cpp: fill EGL shared-context creation + GL bodies
  (see we-fbo-driver/README.md). Then build the lib.
EOF
# cmake -S "$WE_SRC" -B "$WE_SRC/build" -DCMAKE_BUILD_TYPE=Release
# cmake --build "$WE_SRC/build" -j
export WALLPAPERENGINE_INCLUDE_DIR="$WE_SRC/src"

echo "==> [2/4] quickshell @ $QS_COMMIT"
clone_at "$QS_URL" "$QS_SRC" "$QS_COMMIT"
# Overlay the QML module + register it in src/CMakeLists.txt.
rm -rf "$QS_SRC/src/wallpaperengine"
cp -r "$HERE/quickshell-module" "$QS_SRC/src/wallpaperengine"
if ! grep -q 'add_subdirectory(wallpaperengine)' "$QS_SRC/src/CMakeLists.txt"; then
	printf '\nadd_subdirectory(wallpaperengine)\n' >> "$QS_SRC/src/CMakeLists.txt"
fi
# Force Qt Quick onto DESKTOP OpenGL (default is GLES here). WE requires desktop
# GL (EGL_OPENGL_BIT + glew) and GLES<->GL contexts can't share GL objects.
if ! grep -q 'setRenderableType' "$QS_SRC/src/main.cpp"; then
	cat > "$QS_SRC/src/main.cpp" <<'CPP'
#include "launch/main.hpp"

#include <QtGui/QSurfaceFormat>

int main(int argc, char** argv) {
	// linux-wallpaperengine needs a desktop OpenGL context; Qt Quick defaults to
	// GLES here, and GLES<->desktop-GL contexts cannot share GL objects. Force
	// desktop GL so the embedded WE context can share the wallpaper texture.
	auto fmt = QSurfaceFormat::defaultFormat();
	fmt.setRenderableType(QSurfaceFormat::OpenGL);
	fmt.setVersion(4, 5);
	fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
	QSurfaceFormat::setDefaultFormat(fmt);

	return qs::launch::main(argc, argv);
}
CPP
fi
# The module CMakeLists compiles the FBO driver from the WE tree and links the
# installed WE lib; point it at the cloned WE source headers.
export WALLPAPERENGINE_SRC="$WE_SRC/src"
cat <<'EOF'
  DONE automatically: module copied + add_subdirectory(wallpaperengine) added.
  Module CMake mirrors Quickshell's leaf pattern (qt_add_qml_module +
  qs_add_module_deps_light + install_qml_module) and links the installed WE lib.
EOF

echo "==> [3/4] build quickshell"
# The illogical-impulse (end4-pC) shell needs these service plugins compiled in,
# or shell.qml fails to load (e.g. `module "Quickshell.Services.Pipewire" ...
# not found`). They are OFF by default in a plain quickshell build. Configure
# with ALL of them ON in ONE fresh configure - toggling them on an existing
# build dir leaves Qt's dbus codegen half-wired ("No rule to make target
# src/dbus/dbus_objectmanager.cpp"; if that happens, generate it by hand with
# qdbusxml2cpp -p dbus_objectmanager -c DBusObjectManagerInterface
# -i src/dbus/dbus_objectmanager_types.hpp src/dbus/org.freedesktop.DBus.ObjectManager.xml
# into <build>/src/dbus/, same for dbus_properties, then rebuild).
# cmake -S "$QS_SRC" -B "$QS_SRC/build" -DCMAKE_BUILD_TYPE=Release \
#   -DWALLPAPERENGINE_INCLUDE_DIR="$WALLPAPERENGINE_INCLUDE_DIR" \
#   -DSERVICE_MPRIS=ON -DSERVICE_NOTIFICATIONS=ON -DSERVICE_PAM=ON \
#   -DSERVICE_PIPEWIRE=ON -DSERVICE_POLKIT=ON -DSERVICE_STATUS_NOTIFIER=ON \
#   -DSERVICE_UPOWER=ON
# cmake --build "$QS_SRC/build" -j

echo "==> [4/4] package"
cat <<'EOF'
  Package as a patched illogical-impulse-quickshell-git: copy the fork PKGBUILD
    ~/.local/share/ii-vynx-fork/sdata/dist-arch/illogical-impulse-quickshell-git/
  point its source at this patched tree (or add the overlay in prepare()), then
  `makepkg -si`. Keep the pinned _commit in sync with QS_COMMIT above.
EOF

echo "Done (clone + overlay). Finish the MANUAL steps, then uncomment the cmake lines."
