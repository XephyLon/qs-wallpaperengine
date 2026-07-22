# qs-wallpaperengine

Embed **linux-wallpaperengine** directly into **Quickshell** so the shell owns the
live Wallpaper Engine (WE) frames as a Qt texture. This dissolves the whole
"foreign WE surface" problem: widget frosting, lock blur and wallpaper
transitions all become one in-shell path (blur the texture), with no
`session_lock_xray`, no compositor-blur hacks, no separate-surface alignment,
no stretched stills.

This repo is the **build/patch toolchain**, kept out of `~/.config/quickshell`
(which the dots updater wipes). The consuming shell change (a new background
surface that renders `WallpaperEngineSurface` off the existing
`wallpaperSelector.wallpaperEngine.*` config) lives in the shell repo on branch
`feat/embed-wallpaperengine`, which already stripped the old external-process WE
rendering down to selector-only.

## Approach: Option B (patched in-tree Quickshell build)

The running shell is `illogical-impulse-quickshell-git` — upstream Quickshell
pinned at commit `7511545ee20664e3b8b8d3322c0ffe7567c56f7a` (reports as 0.2.1),
built with CMake. We add a native QML module to that source tree and rebuild the
package. Quickshell isn't designed for external C++ plugins, and it already
builds from source here, so in-tree is the pragmatic path.

### Why this is feasible (verified)

- **Quickshell RHI = OpenGL/EGL** (`qs` maps `libEGL`/`libgbm`/`libGLX`, no
  `libvulkan`). WE renders GLES/EGL. Both on EGL → share the texture via a
  shared GL context or `EGLImage`.
- **WE is already a library**: `/opt/linux-wallpaperengine/liblinux-wallpaperengine-lib.so`
  (pkg `linux-wallpaperengine-git`), rendering via GLES/EGL.
- **WE has the right seams** (from `nm -DC` on the lib):
  `WallpaperEngine::Render::Drivers::VideoDriver` (abstract; concrete
  `WaylandOpenGLDriver` / `GLFWOpenGLDriver`), `RenderContext::render(OutputViewport*)`,
  `WallpaperApplication::render()/update(OutputViewport*)`, per-frame
  `CScene/CVideo/CWeb::renderFrame(...)`, and `Render::CFBO` — WE already renders
  to its own FBOs internally.
- **The template**: Quickshell's `ScreencopyView` (`src/wayland/screencopy/view.{hpp,cpp}`)
  is a `QQuickItem` with `QML_ELEMENT` that renders a captured buffer via
  `updatePaintNode()`. `WallpaperEngineSurface` is the same shape, sourcing the
  texture from WE instead of screencopy.

A screencopy-based prototype (WE `--window` toplevel → `ScreencopyView` →
`FastBlur`) already proved the downstream half: a shell-owned texture makes the
frost/lock/transition story trivial. The native embed removes the second process
and the per-frame copy.

## Layout

- `quickshell-module/` — the new Quickshell QML module. Drops into the
  Quickshell source tree as `src/wallpaperengine/`. Exposes
  `Quickshell.WallpaperEngine.WallpaperEngineSurface`, a `QQuickItem` that drives
  WE into a shared FBO and displays its texture. See `INTEGRATION.md`.
- `we-fbo-driver/` — the linux-wallpaperengine patch: a `VideoDriver` /
  `OutputViewport` that renders into a caller-supplied FBO/shared texture instead
  of a wlr-layer-shell surface or GLFW window. This is what lets Quickshell own
  the frames. See its `README.md`.
- `bootstrap.sh` — clones both upstreams at the pinned commits, applies the two
  patch sets, and rebuilds the Quickshell package.

## Build order (see bootstrap.sh)

1. Clone `linux-wallpaperengine` @ the pkg's pinned commit; apply the FBO output
   driver; build/install `liblinux-wallpaperengine-lib.so` with headers.
2. Clone Quickshell @ `7511545e`; drop in `src/wallpaperengine/`; wire
   `add_subdirectory` + link the WE lib; build.
3. Package as a patched `illogical-impulse-quickshell-git` (or `makepkg` with the
   fork PKGBUILD from `~/.local/share/ii-vynx-fork/sdata/dist-arch/`).
4. In the shell repo, add a background surface that instantiates
   `WallpaperEngineSurface { projectPath: Config...activePath; live: true }` and
   feeds it through the existing blur/lock/transition paths.

## Status

Scaffold only. The C++ files are structurally accurate (modeled on
`ScreencopyView`) but the WE-internal wiring (`createContext`, the FBO driver's
hook into `RenderContext`) is stubbed with `TODO`s — those need compile
iteration against WE's real headers, which the from-source build provides.
