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
# NOTE: bumped from 7511545 because that commit fails to build under cmake 4.4
# ("No rule to make target dbus_objectmanager.cpp"); e649d247 builds clean.
# Kept in sync with immaterial-impulse-quickshell-git's _commit. The WE module is
# copied in additively (add_subdirectory), so the CMake side is unaffected by the
# bump — but the module's C++ against e649d247's Quickshell API is UNVERIFIED;
# run a full WE build to confirm before relying on it.
QS_COMMIT="e649d247498512464457aefcd05b73038c4e65a1"   # was 7511545 (cmake-4.4 build break)
WE_URL="https://github.com/Almamu/linux-wallpaperengine"
WE_COMMIT="b016d7d1"                                    # linux-wallpaperengine-git r627.b016d7d1

QS_SRC="$BUILD/quickshell"
WE_SRC="$BUILD/linux-wallpaperengine"

clone_at() { # url dir commit
	local url="$1" dir="$2" commit="$3"
	if [ ! -d "$dir/.git" ]; then git clone "$url" "$dir"; fi
	git -C "$dir" fetch --all --tags
	# -f: discard any local modifications (our idempotent patches leave the tree
	# dirty; a plain checkout would fail and leave a half-reset/half-patched state
	# across re-runs). Start every run from a pristine $commit, then re-patch.
	git -C "$dir" checkout -f --detach "$commit"
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

# WallpaperApplication patches (idempotent):
#  (1) skip setupBrowser() - CEF (Chromium) deadlocks on Quickshell's render
#      thread; scene/video wallpapers don't need it.
#  (2) expose the first wallpaper's scene framebuffer so the host can blit WE's
#      rendered frame (RenderContext renders the scene into the wallpaper's own
#      FBO, not the driver output).
python3 - "$WE_SRC/src/WallpaperEngine/Application/WallpaperApplication.cpp" \
         "$WE_SRC/src/WallpaperEngine/Application/WallpaperApplication.h" <<'PY'
import sys
cpp_p, h_p = sys.argv[1], sys.argv[2]

cpp = open(cpp_p).read()
if "// this->setupBrowser ();" not in cpp:
    cpp = cpp.replace(
        "    this->setupBrowser ();\n",
        "    // EMBED PATCH: setupBrowser() initializes CEF (Chromium), which deadlocks\n"
        "    // when run on Quickshell's render thread (GTK + subprocess message loop).\n"
        "    // Scene/video wallpapers do not need it.\n"
        "    // this->setupBrowser ();\n",
        1,
    )
if "getFirstWallpaperFramebuffer" not in cpp:
    import re
    # Insert right after getDestinationFramebuffer's definition (inside the
    # WallpaperEngine::Application namespace, next to its siblings). Match the
    # line with a regex tolerant of trailing whitespace: some WE revisions have a
    # trailing space after the closing '}', which broke the old exact-string
    # anchor -> .replace() silently no-op'd -> the .h got the declarations but the
    # .cpp had no definitions -> undefined-reference link error. Fail loud if the
    # anchor is genuinely gone rather than silently producing a broken build.
    m = re.search(
        r'^GLuint WallpaperApplication::getDestinationFramebuffer \(\) const \{[^\n]*\}[^\n]*$',
        cpp, re.M)
    if not m:
        sys.exit("EMBED PATCH: getDestinationFramebuffer anchor not found in WallpaperApplication.cpp")
    cpp = cpp[:m.end()] + (
        "\n\nGLuint WallpaperApplication::getFirstWallpaperFramebuffer () const {\n"
        "    const auto& w = this->m_renderContext->getWallpapers ();\n"
        "    return w.empty () ? 0 : w.begin ()->second->getWallpaperFramebuffer ();\n"
        "}\n"
        "int WallpaperApplication::getFirstWallpaperFramebufferWidth () const {\n"
        "    const auto& w = this->m_renderContext->getWallpapers ();\n"
        "    return w.empty () ? 0 : (int) w.begin ()->second->getWallpaperFramebufferWidth ();\n"
        "}\n"
        "int WallpaperApplication::getFirstWallpaperFramebufferHeight () const {\n"
        "    const auto& w = this->m_renderContext->getWallpapers ();\n"
        "    return w.empty () ? 0 : (int) w.begin ()->second->getWallpaperFramebufferHeight ();\n"
        "}"
    ) + cpp[m.end():]
open(cpp_p, "w").write(cpp)

h = open(h_p).read()
if "getFirstWallpaperFramebuffer" not in h:
    anchor = "    [[nodiscard]] GLuint getDestinationFramebuffer () const;\n"
    h = h.replace(anchor, anchor +
        "\n    // Host embedding: the first wallpaper's scene framebuffer (where WE\n"
        "    // renders the final frame) and its real size (getWidth/getHeight -\n"
        "    // video resolution for CVideo, camera projection for CScene). 0 if none.\n"
        "    [[nodiscard]] GLuint getFirstWallpaperFramebuffer () const;\n"
        "    [[nodiscard]] int getFirstWallpaperFramebufferWidth () const;\n"
        "    [[nodiscard]] int getFirstWallpaperFramebufferHeight () const;\n", 1)
open(h_p, "w").write(h)
PY

# CWallpaper: expose the scene FBO's size. GLPlayer resizes the video texture
# but the CFBO object's stored size stays stale, so report the wallpaper's
# logical size (getWidth/getHeight - video resolution for CVideo, camera for
# CScene) which matches the resized texture.
python3 - "$WE_SRC/src/WallpaperEngine/Render/CWallpaper.cpp" \
         "$WE_SRC/src/WallpaperEngine/Render/CWallpaper.h" <<'PY'
import sys
cpp_p, h_p = sys.argv[1], sys.argv[2]
cpp = open(cpp_p).read()
if "getWallpaperFramebufferWidth" not in cpp:
    anchor = "GLuint CWallpaper::getWallpaperFramebuffer () const { return this->m_sceneFBO->getFramebuffer (); }\n"
    cpp = cpp.replace(anchor, anchor +
        "uint32_t CWallpaper::getWallpaperFramebufferWidth () const { return (uint32_t) this->getWidth (); }\n"
        "uint32_t CWallpaper::getWallpaperFramebufferHeight () const { return (uint32_t) this->getHeight (); }\n", 1)
    open(cpp_p, "w").write(cpp)
h = open(h_p).read()
if "getWallpaperFramebufferWidth" not in h:
    anchor = "    [[nodiscard]] virtual GLuint getWallpaperFramebuffer () const;\n"
    h = h.replace(anchor, anchor +
        "    [[nodiscard]] uint32_t getWallpaperFramebufferWidth () const;\n"
        "    [[nodiscard]] uint32_t getWallpaperFramebufferHeight () const;\n", 1)
    open(h_p, "w").write(h)
PY

# Prefer known-good hardware decoders (nvdec on NVIDIA) over mpv's full "auto",
# which tries vdpau first and noisily falls back. Video wallpapers only.
sed -i 's/mpv_set_property_string (this->m_handle, "hwdec", "auto");/mpv_set_property_string (this->m_handle, "hwdec", "auto-safe");/' \
	"$WE_SRC/src/WallpaperEngine/VideoPlayback/MPV/GLPlayer.cpp"

# CScene::dispatchObjectType: guard particle systems with no material. Some
# workshop scenes (e.g. 431960/1955123321 "2B") ship a particle object whose
# material is null; CParticle's constructor dereferences particle.material->material
# unconditionally, so a missing material is a hard SIGSEGV. Embedded, that crash
# takes the whole Quickshell host down (not just the wallpaper). Skip the particle
# instead. Idempotent (guarded by the marker string).
python3 - "$WE_SRC/src/WallpaperEngine/Render/Wallpapers/CScene.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
if "Skipping particle system with no material" not in s:
    anchor = "\trenderObject = new Objects::CParticle (*this, particleData);"
    guard = (
        "\t// EMBED PATCH: some scenes ship a particle object with no material (or an\n"
        "\t// empty model wrapper). CParticle's constructor dereferences\n"
        "\t// particle.material->material unconditionally, so a missing material is a\n"
        "\t// hard SIGSEGV that takes the whole host process down. Skip it instead.\n"
        "\tif (particleData.material == nullptr || particleData.material->material == nullptr) {\n"
        "\t    sLog.error (\"Skipping particle system with no material: \", particleData.name);\n"
        "\t    return nullptr;\n"
        "\t}\n\n"
    )
    if anchor not in s:
        sys.exit("EMBED PATCH: CParticle dispatch anchor not found in CScene.cpp")
    s = s.replace(anchor, guard + anchor, 1)
    open(p, "w").write(s)
PY

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
#   -DSERVICE_UPOWER=ON -DBLUETOOTH=ON
# cmake --build "$QS_SRC/build" -j

echo "==> [4/4] package"
cat <<'EOF'
  Package as a patched illogical-impulse-quickshell-git: copy the fork PKGBUILD
    ~/.local/share/ii-vynx-fork/sdata/dist-arch/illogical-impulse-quickshell-git/
  point its source at this patched tree (or add the overlay in prepare()), then
  `makepkg -si`. Keep the pinned _commit in sync with QS_COMMIT above.
EOF

echo "Done (clone + overlay). Finish the MANUAL steps, then uncomment the cmake lines."
