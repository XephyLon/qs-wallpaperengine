# Changelog

qs-wallpaperengine follows [Semantic Versioning](https://semver.org/) (currently
pre-1.0: `0.x` may change without notice). The current version is in `VERSION`.

## [Unreleased]

### Fixed
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
