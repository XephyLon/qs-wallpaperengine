# Changelog

qs-wallpaperengine follows [Semantic Versioning](https://semver.org/) (currently
pre-1.0: `0.x` may change without notice). The current version is in `VERSION`.

## [0.2.0] — 2026-08-03

### Added
- `WallpaperEngineSurface.failed` — true when the renderer cannot render the
  current project at all: the WE thread failing to start, GLEW or context
  setup failing, `setup()` throwing, no driver registering, or the render
  targets coming back `INCOMPLETE`. Nothing is ever drawn into the surface in
  that state, so without this a failed wallpaper was a black desktop with a
  clean log and `rendered == true`. Consumers can watch it and fall back to a
  static image.

  Deliberately **not** driven by `glGetError`. That was measured first: the
  `INVALID_FRAMEBUFFER_OPERATION` reported after mpv creates its texture fires
  on every video-wallpaper start here and the video then plays perfectly —
  linux-wallpaperengine never drains the error queue in its render path, so a
  stale error survives and gets misattributed. A `glGetError`-driven detector
  would black out every working video wallpaper.

### Fixed
- Up to ~118 MB of VRAM leaked per failed wallpaper load. Five early returns in
  `WeThread::run()` skipped teardown of the full-screen RGBA8 colour target and
  its D24S8 renderbuffer, which live in the process-global share group and so
  outlived the context that created them. Enough failures and allocations begin
  failing for *healthy* wallpapers — indistinguishable from "one bad wallpaper
  corrupts the rest", with no corruption involved.
- A wedged WE thread could corrupt GL state shared with Qt. `~WeThread` gives up
  after a 3s join and detaches, but the caller then destroyed the
  `QOpenGLContext` anyway — leaving a live thread issuing GL against a
  destroyed context in a namespace shared with Qt. The context is now
  deliberately leaked in that case, which is the cheaper of the two outcomes.

### Changed
- Repaints are driven by frame production rather than a timer. The old
  `QTimer` interval was integer `1000/fps` ms, so the consumer clock never
  matched the producer's — fps=24 polled at 24.4Hz, fps=144 at 166Hz, fps=90 at
  90.9Hz — repeating unchanged frames (a full-screen commit and recomposite
  each) while dropping fresh ones. `WeThread` now invokes an `onFrame` callback
  on publish and on every failure path, hopped to the GUI thread through a
  shared `WeFrameSink` so a detached thread can never post through a dangling
  `this`; posts coalesce so a stalled GUI cannot queue up wake-ups. A 1Hz poll
  remains but fires only when the producer has gone silent, preserving the old
  timer's safety role (context-loss rebuild, failure latch) at no cost while
  frames flow.

## [0.1.0] — 2026-08-03

### Fixed
- A live wallpaper froze on a still frame whenever any window anywhere was
  fullscreen — including one parked on a workspace the user had left — and
  stayed frozen until it stopped being fullscreen. The embedded WE ran
  linux-wallpaperengine's own fullscreen pause (`pauseOnFullscreen` defaults
  on), whose detector counts EVERY fullscreen toplevel the compositor
  advertises: output, workspace and visibility are not part of the test.
  `WallpaperApplication::render()` then early-returns and pauses mpv with it.
  The embed now passes `--fullscreen-pause-only-active`, which narrows that
  count to *activated* toplevels — a window holds activation exactly while it
  is focused, which is exactly while it covers the wallpaper. So a live
  wallpaper idles behind a fullscreen window and animates whenever it is on
  screen. (The shell cannot own this policy itself: its own suppression stops
  Qt drawing the surface and `live` only gates the repaint timer, neither of
  which reaches the WE thread — only WE's `setPause()` reaches mpv.)
- Wallpaper black-outs and wedged (permanently black) video wallpapers while a
  game runs: Hyprland's fullscreen direct-scanout recreates Qt's scene-graph
  GL contexts several times a minute in a game session, and every recreation
  used to tear down and rebuild the whole WE thread — seconds of black each
  time, and mpv teardown under GPU contention could overrun the join deadline,
  detach the old thread and leave two WE instances fighting over process
  globals (black until shell restart). The patched quickshell now sets
  `AA_ShareOpenGLContexts` (one process-global GL share group) and the module
  shares its WE context against `QOpenGLContext::globalShareContext()`, so
  scene-graph context recreation no longer invalidates the wallpaper texture
  and the WE thread survives untouched. Verified: repeated fullscreen cycles
  with a running game produce zero rebuilds. The stale-node drop and
  context-lost latch remain as the fallback for stock builds without a global
  share context.
- Strobing/garbage wallpaper after leaving fullscreen: when Hyprland's
  direct-scanout made Qt recreate the scene-graph GL context, the surface kept
  drawing its old node, whose texture wraps a GL texture *name* from the
  destroyed context's share group — dangling or recycled for unrelated
  textures in the new one. The node is now dropped on context change; the
  wallpaper blanks briefly while the WE thread rebuilds instead of strobing.
- The context-change detection itself was unreliable: it compared
  `QOpenGLContext` pointers, and a destroy+recreate routinely reuses the same
  heap address, so the rebuild never fired and WE's texture names aliased
  unrelated textures in the new share group (observed as a desktop widget's
  cached layer drawn fullscreen as the wallpaper). The adopted context's
  `aboutToBeDestroyed` now latches a context-lost flag that forces the
  rebuild regardless of pointer identity.

## [0.1.0] — 2026-07-23

### Added
- Initial versioned snapshot of the build/patch toolchain that embeds
  linux-wallpaperengine into Quickshell so the shell owns the live WE frames as
  a Qt texture (`WallpaperEngineSurface`).
- Consumed by the Immaterial Impulse installer, which pins the revision it
  builds (its `WE_REF`).
