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

## Files (reconciled against the real WE headers)

The real structure is three classes — `VideoDriver → Output → OutputViewport`
(my first pass conflated them). Modeled 1:1 on the GLFW windowed path:

- `CFboOpenGLDriver.h/.cpp` — `: VideoDriver`. Owns the shared EGL context and
  the output; `dispatchEventQueue()` renders one frame into the FBO (clear +
  `getApp().update(viewport)` per viewport) and **omits** `glfwSwapBuffers` /
  `glfwPollEvents` / the FPS `usleep` — the host owns present + pacing.
  Exposes `texture()` (FBO color attachment) for the host.
- `CFboWindowOutput.h/.cpp` — `: Output`. Owns the FBO (color texture +
  depth/stencil), one viewport. `renderVFlip()=true` (GL bottom-up),
  `haveImageBuffer()=false` (texture, not CPU readback), `updateRender()` no-op.
- `CFboOutputViewport.h/.cpp` — `: OutputViewport`. `makeCurrent()` binds the FBO
  and sets `glViewport`; `swapOutput()` flushes.

Verified virtuals: `VideoDriver` = getOutput/getRenderTime/closeRequested/
resizeWindow×2/show|hideWindow/getFramebufferSize/getFrameCounter/getProcAddress/
dispatchEventQueue (ctor takes `WallpaperApplication&` + `Input::MouseInput&`).
`Output` = reset/renderVFlip/renderMultiple/haveImageBuffer/getImageBuffer/
getImageBufferSize/updateRender. `OutputViewport` = makeCurrent/swapOutput.

## Remaining TODOs (the real compile-iteration work)

1. **Mouse input**: `VideoDriver`'s ctor needs an `Input::MouseInput&`. GLFW
   passes `GLFWMouseInput`; a headless FBO needs a null one — check
   `Input/MouseInput.h` for a usable concrete or add `NullMouseInput`.
2. **EGL shared context**: create the WE `EGLContext` with the host's as
   `share_context` so `texture()` is valid in Qt's context (see main README §
   "Context sharing"). Feed WE's `getRenderTime()` from the host frame clock so
   animation steps with Qt.
3. Fill the GL bodies (FBO alloc in `ensureFbo`, bind in `makeCurrent`, clear in
   `dispatchEventQueue`).

## Applying

`bootstrap.sh` copies the driver into `src/WallpaperEngine/Render/Drivers/` and
the output/viewport into `.../Drivers/Output/`. Add the four `.cpp` to the driver
sources in `src/CMakeLists.txt`. For the embed, Quickshell links the lib and
constructs `CFboOpenGLDriver` directly with the host EGL display/context.
