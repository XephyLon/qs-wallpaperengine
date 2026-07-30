# Changelog

qs-wallpaperengine follows [Semantic Versioning](https://semver.org/) (currently
pre-1.0: `0.x` may change without notice). The current version is in `VERSION`.

## [Unreleased]

### Fixed
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
