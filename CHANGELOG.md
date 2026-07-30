# Changelog

qs-wallpaperengine follows [Semantic Versioning](https://semver.org/) (currently
pre-1.0: `0.x` may change without notice). The current version is in `VERSION`.

## [Unreleased]

## [0.1.0] — 2026-07-23

### Added
- Initial versioned snapshot of the build/patch toolchain that embeds
  linux-wallpaperengine into Quickshell so the shell owns the live WE frames as
  a Qt texture (`WallpaperEngineSurface`).
- Consumed by the Immaterial Impulse installer, which pins the revision it
  builds (its `WE_REF`).
