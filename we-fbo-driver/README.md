# linux-wallpaperengine FBO output driver

Adds a render backend that draws a WE wallpaper into a caller-supplied
**FBO / shared GL texture** instead of a wlr-layer-shell surface (`WaylandOpenGLDriver`)
or a GLFW window (`GLFWOpenGLDriver`). This is what lets Quickshell own the WE
frames: the FBO's color attachment is a GL texture Quickshell imports as a
`QSGTexture`.

## Where it hooks in (from `nm -DC` on liblinux-wallpaperengine-lib.so)

- `WallpaperEngine::Render::Drivers::VideoDriver` — abstract output backend.
  Concrete siblings: `WaylandOpenGLDriver`, `GLFWOpenGLDriver`. **CFboOutput is a
  new sibling.** It owns (or adopts) the EGL context and presents to an FBO.
- `WallpaperEngine::Render::Drivers::Output::OutputViewport` — the render-target
  region. `RenderContext::render(OutputViewport*)` renders the active wallpaper
  into it. CFboOutput exposes one viewport sized to the wallpaper.
- `WallpaperEngine::Render::RenderContext::RenderContext(VideoDriver&, WallpaperApplication&, MediaSource&)`
  — construct with our driver.
- `WallpaperEngine::Render::CFBO` — WE already wraps FBOs; reuse it for the
  target so scenes composite the same way they do for the on-screen drivers.

## Context sharing (the crux)

Quickshell runs the OpenGL RHI on EGL. CFboOutput must **not** create its own
isolated context — it must render on a context that shares objects with
Quickshell's `QSGRenderContext`, so the FBO texture is valid in Qt's context.
Two options:

1. **Shared EGLContext**: create the WE `EGLContext` with Quickshell's context as
   `share_context` (same `EGLDisplay`, compatible config). Texture ids are then
   valid in both. Render WE on Quickshell's render thread (`beforeRendering`),
   making Quickshell's context current, or a shared one.
2. **EGLImage**: render WE in its own context to a texture backed by an
   `EGLImage` (dmabuf), import that image into a Qt texture. More moving parts;
   only needed if a shared context proves impractical.

Start with (1); the surface (`../quickshell-module`) drives `renderFrame()` from
`QQuickWindow::beforeRendering` with Qt's GL context current.

## Files

- `CFboOutput.hpp/.cpp` — the VideoDriver subclass + its OutputViewport.
  Method signatures are stubbed `TODO` until reconciled against the real headers
  the from-source build exposes (`WallpaperEngine/Render/Drivers/CVideoDriver.h`
  and friends — the packaged `-git` ships only vendored-dep headers, not these).

## Applying

`bootstrap.sh` copies these into the WE source under
`src/WallpaperEngine/Render/Drivers/`, registers `CFboOutput` in the driver
CMake list, and (optionally) adds a `--output fbo` selection path so the same
binary can still run standalone for testing. For the embed, Quickshell links the
lib and constructs `CFboOutput` directly.
