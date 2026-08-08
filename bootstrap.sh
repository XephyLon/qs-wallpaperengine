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

# WallpaperApplication: external pause control (qs-wallpaperengine#19).
# `occluded` idles the module's render thread but nothing stopped mpv decoding:
# the only path that pauses playback is render()'s fullscreen-detector branch
# (m_renderContext->setPause -> CVideo -> GLPlayer -> mpv `pause`), and
# --no-fullscreen-pause stubs that detector out because it is output-blind
# (process-wide) while the shell runs one renderer per output. Give the host a
# per-instance flag ORed into BOTH detector checks, so the shell's per-output
# occlusion verdict drives WE's own pause machinery - playlist timer
# accounting and clean resume included - with the stub detector still off.
python3 - "$WE_SRC/src/WallpaperEngine/Application/WallpaperApplication.cpp" \
         "$WE_SRC/src/WallpaperEngine/Application/WallpaperApplication.h" <<'PY'
import sys
cpp_p, h_p = sys.argv[1], sys.argv[2]

cpp = open(cpp_p).read()
if "m_externalPaused" not in cpp:
    cond = ("this->m_fullScreenDetector->anythingFullscreen () "
            "&& this->m_context.state.general.keepRunning")
    # Both the enter-pause check and the stay-paused check in render(). Exactly
    # two, or the anchor drifted and the patch no longer means what it says.
    if cpp.count(cond) != 2:
        sys.exit("EMBED PATCH: expected exactly 2 fullscreen-detector checks "
                 f"in WallpaperApplication.cpp render(), found {cpp.count(cond)}")
    cpp = cpp.replace(cond,
        "(this->m_fullScreenDetector->anythingFullscreen () || this->m_externalPaused) "
        "&& this->m_context.state.general.keepRunning")
open(cpp_p, "w").write(cpp)

h = open(h_p).read()
if "setExternalPaused" not in h:
    anchor = "    [[nodiscard]] int getFirstWallpaperFramebufferHeight () const;\n"
    if anchor not in h:
        sys.exit("EMBED PATCH: getFirstWallpaperFramebufferHeight anchor not "
                 "found in WallpaperApplication.h (apply the framebuffer patch first)")
    h = h.replace(anchor, anchor +
        "\n    // EMBED PATCH (qs-wallpaperengine#19): host-driven pause. ORed into\n"
        "    // render()'s fullscreen-detector checks so the embedding shell's\n"
        "    // per-output occlusion verdict runs WE's own pause machinery (the\n"
        "    // only thing that stops mpv decoding). Same-thread as render();\n"
        "    // plain bool on purpose.\n"
        "    void setExternalPaused (bool paused) { this->m_externalPaused = paused; }\n"
        "    bool m_externalPaused = false;\n", 1)
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

# GLPlayer: cap libmpv's vsync fences (#16). mpv's vo=libmpv creates one
# glFenceSync per rendered frame in ra_gl_ctx_submit_frame and never deletes
# it - the only cleanup lives in ra_gl_ctx_swap_buffers, which the render API
# deliberately never calls (libmpv_gl.c: "we can just not call them to begin
# with"). Each undeleted sync pins host memory in the GL driver (~5 KB on
# NVIDIA), which leaks ~10 MB/min at 30 fps for EVERY video wallpaper,
# regardless of resolution or hardware/software decode. Interpose
# glFenceSync/glDeleteSync in the get_proc_address table handed to mpv and
# delete the oldest sync beyond a small cap; mpv's own PBO-pool fences are
# waited and deleted young, so only the leaked ones age into it. Verified:
# harness slope 10.2 MB/min -> 0.6 MB/min, and flat (56 KB/min) over a 12-min
# standalone run. Idempotent (marker-guarded); fails loud on anchor loss.
python3 - "$WE_SRC/src/WallpaperEngine/VideoPlayback/MPV/GLPlayer.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
if "EMBED PATCH: cap libmpv vsync fences" in s:
    sys.exit(0)

inc_anchor = '#include <mpv/stream_cb.h>\n'
if inc_anchor not in s:
    sys.exit("EMBED PATCH: stream_cb include anchor not found in GLPlayer.cpp")
s = s.replace(inc_anchor, inc_anchor + '\n#include <algorithm>\n#include <cstring>\n#include <deque>\n#include <mutex>\n', 1)

old_fn = '''void* get_proc_address (void* ctx, const char* name) {
    return static_cast<GLPlayer*> (ctx)->getContext ().getDriver ().getProcAddress (name);
}'''
if old_fn not in s:
    sys.exit("EMBED PATCH: get_proc_address anchor not found in GLPlayer.cpp")
new_fn = '''// EMBED PATCH: cap libmpv vsync fences (qs-wallpaperengine#16).
// mpv's vo=libmpv creates one glFenceSync per rendered frame in
// ra_gl_ctx_submit_frame and never deletes it: the only cleanup lives in
// ra_gl_ctx_swap_buffers, which the render API deliberately never calls
// (libmpv_gl.c: "we can just not call them to begin with"). Each undeleted
// sync pins host memory in the GL driver (~5 KB on NVIDIA), leaking ~10 MB/min
// at 30 fps for every video wallpaper. mpv never touches the aged fences again
// (no wait, no delete, not even at uninit), so deleting the oldest beyond a
// cap is safe; mpv's own PBO-pool fences are waited and deleted young, so only
// the leaked ones ever age into the cap.
namespace {
std::mutex s_fenceLock;
std::deque<void*> s_fenceRing;
constexpr size_t FENCE_RING_CAP = 64;
using FenceSyncFn = void* (*) (unsigned int, unsigned int);
using DeleteSyncFn = void (*) (void*);
FenceSyncFn s_realFenceSync = nullptr;
DeleteSyncFn s_realDeleteSync = nullptr;

void* cappedFenceSync (unsigned int condition, unsigned int flags) {
    void* sync = s_realFenceSync (condition, flags);
    if (sync == nullptr) return sync;
    void* victim = nullptr;
    {
	std::lock_guard lock (s_fenceLock);
	s_fenceRing.push_back (sync);
	if (s_fenceRing.size () > FENCE_RING_CAP) {
	    victim = s_fenceRing.front ();
	    s_fenceRing.pop_front ();
	}
    }
    if (victim != nullptr) s_realDeleteSync (victim);
    return sync;
}

void cappedDeleteSync (void* sync) {
    {
	std::lock_guard lock (s_fenceLock);
	const auto it = std::find (s_fenceRing.begin (), s_fenceRing.end (), sync);
	if (it != s_fenceRing.end ()) s_fenceRing.erase (it);
    }
    s_realDeleteSync (sync);
}

// Drain on player stop, while a context of the share group is still current:
// ring entries from a fully destroyed standalone context would otherwise be
// deleted later through dangling sync handles.
void drainFenceRing () {
    std::deque<void*> doomed;
    {
	std::lock_guard lock (s_fenceLock);
	doomed.swap (s_fenceRing);
    }
    if (s_realDeleteSync != nullptr)
	for (void* sync : doomed) s_realDeleteSync (sync);
}
} // namespace

void* get_proc_address (void* ctx, const char* name) {
    void* proc = static_cast<GLPlayer*> (ctx)->getContext ().getDriver ().getProcAddress (name);
    if (proc != nullptr && std::strcmp (name, "glFenceSync") == 0) {
	s_realFenceSync = reinterpret_cast<FenceSyncFn> (proc);
	return reinterpret_cast<void*> (&cappedFenceSync);
    }
    if (proc != nullptr && std::strcmp (name, "glDeleteSync") == 0) {
	s_realDeleteSync = reinterpret_cast<DeleteSyncFn> (proc);
	return reinterpret_cast<void*> (&cappedDeleteSync);
    }
    return proc;
}'''
s = s.replace(old_fn, new_fn, 1)

stop_anchor = '''void GLPlayer::stop () {
    // clean up mpv and get it ready to start again at some point
    if (this->m_renderContext) {'''
if stop_anchor not in s:
    sys.exit("EMBED PATCH: GLPlayer::stop anchor not found in GLPlayer.cpp")
s = s.replace(stop_anchor, '''void GLPlayer::stop () {
    // EMBED PATCH (qs-wallpaperengine#16): see drainFenceRing above.
    drainFenceRing ();
    // clean up mpv and get it ready to start again at some point
    if (this->m_renderContext) {''', 1)

open(p, "w").write(s)
PY

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

# JsonExtensions::optional<T>: tolerate string-typed numbers. Some workshop
# wallpapers store numeric fields as strings ("pointsize": "16"); the implicit
# json->T conversion in these noexcept accessors then throws type_error.302,
# which std::terminate()s the whole embedded Quickshell host (seen via
# ObjectParser::parseText -> optional<int>). Coerce numeric strings, otherwise
# log and fall back to the default/nullopt. Idempotent (marker-guarded).
python3 - "$WE_SRC/src/WallpaperEngine/Data/JSON.h" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
if "EMBED PATCH: tolerate string-typed numbers" not in s:
    old_nullopt = "\tif (it == base.end () || it->is_null ()) {\n\t    return std::nullopt;\n\t}\n\n\treturn *it;\n    }"
    new_nullopt = (
        "\tif (it == base.end () || it->is_null ()) {\n\t    return std::nullopt;\n\t}\n\n"
        "\t// EMBED PATCH: tolerate string-typed numbers - some workshop wallpapers\n"
        "\t// store numeric fields as strings; the implicit conversion throws inside\n"
        "\t// this noexcept accessor, which aborts the whole embedded host process.\n"
        "\ttry {\n"
        "\t    return *it;\n"
        "\t} catch (const std::exception&) {\n"
        "\t    if constexpr (std::is_arithmetic_v<T>) {\n"
        "\t\tif (it->is_string ()) {\n"
        "\t\t    try { return static_cast<T> (std::stod (it->template get<std::string> ())); } catch (...) {}\n"
        "\t\t}\n"
        "\t    }\n"
        "\t    sLog.error (\"Invalid value type for key \", key, \", ignoring. Value: \", it->dump ());\n"
        "\t    return std::nullopt;\n"
        "\t}\n    }"
    )
    old_default = "\tif (it == base.end () || it->is_null ()) {\n\t    return defaultValue;\n\t}\n\n\treturn (*it);\n    }"
    new_default = (
        "\tif (it == base.end () || it->is_null ()) {\n\t    return defaultValue;\n\t}\n\n"
        "\t// EMBED PATCH: see optional(key) above - same string-typed-number hazard.\n"
        "\ttry {\n"
        "\t    return (*it);\n"
        "\t} catch (const std::exception&) {\n"
        "\t    if constexpr (std::is_arithmetic_v<T>) {\n"
        "\t\tif (it->is_string ()) {\n"
        "\t\t    try { return static_cast<T> (std::stod (it->template get<std::string> ())); } catch (...) {}\n"
        "\t\t}\n"
        "\t    }\n"
        "\t    sLog.error (\"Invalid value type for key \", key, \", using default. Value: \", it->dump ());\n"
        "\t    return defaultValue;\n"
        "\t}\n    }"
    )
    if old_nullopt not in s or old_default not in s:
        sys.exit("EMBED PATCH: JsonExtensions::optional anchors not found in JSON.h")
    s = s.replace(old_nullopt, new_nullopt, 1).replace(old_default, new_default, 1)
    if "#include <type_traits>" not in s:
        s = s.replace("#include <string>\n", "#include <string>\n#include <type_traits>\n", 1)
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
if ! grep -q 'AA_ShareOpenGLContexts' "$QS_SRC/src/main.cpp"; then
	cat > "$QS_SRC/src/main.cpp" <<'CPP'
#include "launch/main.hpp"

#include <QtCore/QCoreApplication>
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

	// One process-global GL share group: every scene-graph context Qt creates
	// shares objects with QOpenGLContext::globalShareContext(). The WE module
	// builds its render context against that global context, so a
	// compositor-forced scene-graph context recreation (Hyprland fullscreen
	// direct scanout) no longer orphans the wallpaper texture - and the WE
	// thread never needs a rebuild for it.
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

	return qs::launch::main(argc, argv);
}
CPP
fi
# Desktop-entry monitor: upstream watches the applications dirs AND every
# parent up to / (so a missing applications dir's creation is noticed). But it
# rescans ALL .desktop files on a change to ANY watched dir — including parents
# like ~/.local/share, /usr/share and $HOME. Theme tools (kde-material-you's
# plasma-apply-colorscheme / plasma-changeicons writing color-schemes/, konsole/
# profiles) then trigger full rescans on every wallpaper switch; each rescan
# rebuilds large QML arrays whose Qt 6.11 QV4 garbage collection pauses the GUI
# thread for seconds (measured 2-8s per burst, recurring). Only rescan for
# changes inside a real applications dir, or when a missing one appears.
# Idempotent (guarded by the marker string).
python3 - "$QS_SRC/src/core/desktopentrymonitor.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
if "must NOT trigger a rescan" not in s:
    old = (
        "void DesktopEntryMonitor::onDirectoryChanged(const QString& /*path*/) {\n"
        "\tthis->debounceTimer.start();\n"
        "}\n"
    )
    new = (
        "void DesktopEntryMonitor::onDirectoryChanged(const QString& path) {\n"
        "\t// EMBED PATCH: parent directories (up to /) are watched ONLY so the\n"
        "\t// creation of a missing applications dir is noticed. A change reported\n"
        "\t// for a parent (e.g. ~/.local/share when a color-scheme or any sibling\n"
        "\t// file lands) must NOT trigger a rescan of every .desktop file: that\n"
        "\t// rescan cascades into large QML array rebuilds whose garbage\n"
        "\t// collection pauses the UI for seconds. Only rescan when the change is\n"
        "\t// in an actual applications dir (or a subdir), or when a previously\n"
        "\t// missing applications dir just appeared.\n"
        "\tfor (const auto& desktopPath: DesktopEntryManager::desktopPaths()) {\n"
        "\t\tif (path == desktopPath || path.startsWith(desktopPath + QChar(u'/'))) {\n"
        "\t\t\tthis->debounceTimer.start();\n"
        "\t\t\treturn;\n"
        "\t\t}\n"
        "\t}\n"
        "\n"
        "\t// Parent changed: pick up newly-created applications dirs.\n"
        "\tfor (const auto& desktopPath: DesktopEntryManager::desktopPaths()) {\n"
        "\t\tif (!this->watcher.directories().contains(desktopPath) && QDir(desktopPath).exists()) {\n"
        "\t\t\taddPathAndParents(this->watcher, desktopPath);\n"
        "\t\t\tthis->scanAndWatch(desktopPath);\n"
        "\t\t\tthis->debounceTimer.start();\n"
        "\t\t\treturn;\n"
        "\t\t}\n"
        "\t}\n"
        "}\n"
    )
    if old not in s:
        sys.exit("EMBED PATCH: onDirectoryChanged anchor not found in desktopentrymonitor.cpp")
    s = s.replace(old, new, 1)
    open(p, "w").write(s)
PY

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
