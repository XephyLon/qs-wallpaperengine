# Changelog

qs-wallpaperengine follows [Semantic Versioning](https://semver.org/) (currently
pre-1.0: `0.x` may change without notice). The current version is in `VERSION`.

## [Unreleased]

### Added
- **The rest of the engine's flag set on `WallpaperEngineSurface`.** Six new
  properties, all load-time WE arguments that reload the wallpaper on change,
  exactly like `scaleMode`/`audioEnabled`: `volume` (0..100, mapped onto WE's
  0..128 scale; 100 is the old fixed "full internal volume" so nothing changes
  for existing consumers), `audioProcessing` (WE's audio-reactive recorder -
  independent of `audioEnabled`, so a muted wallpaper can still react, and
  placed outside the `--volume`/`--silent` branch for that reason),
  `mouseDisabled`, `parallaxDisabled`, `particlesDisabled` (WE's
  `--disable-mouse`/`--disable-parallax`/`--disable-particles`), and
  `properties` - a map of project.json `general.properties` overrides in
  `--set-property`'s own `name=value` string form, sorted by key so the same
  map always builds the same argv. The reload dance the setters share
  (generation retire, loaded-path clear, verdict take-back) is one
  `retireAndReload()` now instead of a third and fourth hand copy.
- **A live crop focus for the "fill" scale mode, and the content size to
  drive it.** `focusX`/`focusY` (0..1, 0.5 = centre) move where the cover-crop
  sits when a scene overflows the output on an axis - LIVE, pushed straight to
  the render thread and read every frame in the scene-FBO blit, so a UI can pan
  the wallpaper with no reload. It reaches only the scene path (a video is
  composited by WE, which has no crop offset, so it stays centre-cropped).
  `contentWidth`/`contentHeight` publish the scene's authored size (0 for a
  video or before the first frame), so a crop picker can size its content box
  by the real aspect rather than the preview image's - a 32:9 wallpaper
  commonly ships a portrait preview.
- **A full-scene grab for the crop picker.** `requestSceneGrab(path)` writes
  the uncropped scene FBO to a PNG at its real authored aspect (e.g. 5120x1440
  = 32:9) on the render thread's next frame, then emits `sceneGrabReady(path)`.
  A crop picker needs this because the preview image is not the scene: a 32:9
  ultrawide wallpaper commonly ships a portrait preview, so the picker cannot
  represent the wallpaper from it. A no-op for a video (no scene FBO).

## [0.2.6] — 2026-08-08

### Fixed
- **`occluded` now pauses video playback, not just publishing.**
  ([#19](https://github.com/XephyLon/qs-wallpaperengine/issues/19)) Since
  v0.2.2 disabled WE's own (output-blind) fullscreen detector, nothing paused
  mpv: `occluded` skipped the blit/fence/publish but a software-decoded video
  kept burning CPU behind a fullscreen game (~180% measured for 7680x2160
  h264, which NVDEC rejects - its h264 width cap is 4096). The bootstrap patch
  gives `WallpaperApplication` a per-instance `setExternalPaused()` ORed into
  both of `render()`'s fullscreen-detector checks, and the render thread
  forwards the occlusion flag into it every iteration - WE's own pause
  machinery (mpv `pause`, playlist-timer accounting, clean resume) runs
  exactly as it does standalone. Measured: ~250% -> ~1.5% CPU on occlude,
  full-rate resume on uncover, across repeated cycles.

## [0.2.5] — 2026-08-08

### Fixed
- **Rebuild against ffmpeg 9 / x265 4.3.** No code changes. The v0.2.4 prebuilt
  links the system ffmpeg 8 sonames (`libavcodec.so.62`, `libavformat.so.62`,
  `libavutil.so.60`, `libswresample.so.6`); Arch's ffmpeg `2:9.0` soname bump
  removes them, so `quickshell` fails at load with
  `libavcodec.so.62: cannot open shared object file` the moment the system
  updates. This release's prebuilt is produced by the same CI recipe on the
  updated `archlinux:latest` image and links the ffmpeg 9 sonames
  (`.63`/`.63`/`.61`/`.7`). Only `liblinux-wallpaperengine-lib.so` links ffmpeg
  directly; mpv resolves via its own rebuilt package.

## [0.2.4] — 2026-08-07

### Fixed
- **Every video wallpaper leaked ~10 MB/min of host memory, forever.**
  ([#16](https://github.com/XephyLon/qs-wallpaperengine/issues/16)) mpv's
  `vo=libmpv` creates one `glFenceSync` per rendered frame in
  `ra_gl_ctx_submit_frame` and never deletes it: the only cleanup lives in
  `ra_gl_ctx_swap_buffers`, which the render API deliberately never calls
  (`libmpv_gl.c`). Each undeleted sync pins ~5 KB of driver-side host memory on
  NVIDIA — at 30 fps that is ~10 MB/min, ~14 GB/day, independent of resolution
  and of hardware vs software decode, and GL sync objects belong to the share
  group, so switching wallpapers never reclaimed them. Diagnosed with a
  standalone libmpv repro (no WE, no Quickshell) plus a GL call-count shim:
  fences created 30/s, `glDeleteSync` called zero times, ~5.2 KB apiece —
  matching the observed slope exactly. The bootstrap now patches GLPlayer to
  interpose `glFenceSync`/`glDeleteSync` in the function table handed to mpv
  and delete the oldest sync beyond a 64-entry cap (mpv's own PBO-pool fences
  are waited and deleted young, so only leaked fences age into it), and to
  drain the ring on player stop. Measured: embedded harness slope 10.2 MB/min
  → 0.6 MB/min; a 12-minute standalone run settles flat at ~56 KB/min after
  the first loop restart. Scene wallpapers were never affected. Upstream mpv
  bug; a report with the minimal repro is being prepared separately.
- Disabling wallpaper audio no longer leaves linux-wallpaperengine's automute
  detector running. `--silent` suppresses playback but does not disable that
  detector; it was synchronously enumerating PulseAudio and every sink input on
  every rendered frame despite there being no wallpaper audio to mute. A
  heaptrack run made while investigating
  [#16](https://github.com/XephyLon/qs-wallpaperengine/issues/16) recorded 10.7
  million PulseAudio allocations in 150 seconds from this path. Removing that
  churn does not by itself resolve #16's remaining video-path growth. The embed
  now always passes `--noautomute`; its own `audioEnabled` property remains the
  single audio policy, while enabled wallpapers still use `--volume 128` and
  disabled ones use `--silent`.

## [0.2.3] — 2026-08-05

### Fixed
- **The shipped binary could not find its own bundled libraries.** Release
  tarballs carried the builder's own directory as their RUNPATH — in CI,
  `/__w/qs-wallpaperengine/...` — a path that exists on no user's machine, so
  `bin/quickshell` could not resolve the `lib/*.so` sitting beside it in the very
  same archive ([#12](https://github.com/XephyLon/qs-wallpaperengine/issues/12)).

  Consumers had to work around it, and the workaround leaked: immaterial-impulse
  rewrote the RUNPATH with `patchelf`, and where `patchelf` was missing it
  exported `LD_LIBRARY_PATH` instead — which is inherited by every process the
  shell spawns, so CEF's bundled `libEGL`/`libGLESv2` shadowed the system ones
  for every application launched from it.

  Packaging now sets `$ORIGIN/../lib` on the binary and
  `$ORIGIN:/opt/linux-wallpaperengine/...` on every bundled library, and
  **refuses to produce a tarball** whose RUNPATHs are anything else. Both are
  needed: `DT_RUNPATH` is not transitive, so the executable's path resolves only
  its own direct dependencies - `liblinux-wallpaperengine-lib.so` finds
  `libcef.so` beside it through *its* path, which was also the builder's. This shipped in every release through 0.2.2 and is
  invisible in the artifact unless something looks, so the check lives where the
  artifact is made.

## [0.2.2] — 2026-08-04

### Added
- `WallpaperEngineSurface.occluded` — set by the shell when a fullscreen window
  covers **that** output. While it is set the renderer idles at a few frames a
  second and publishes nothing, so the surface stops repainting and the window
  stops committing; clearing it produces a fresh frame within a frame or two. It
  is a live toggle: unlike `scaleMode` / `audioEnabled` it does not reload the
  wallpaper.

  **This supersedes the fullscreen-pause entry under 0.1.0 below**, including its
  claim that "the shell cannot own this policy itself". It can, and it is the
  only side that can: linux-wallpaperengine's detector has no concept of an
  output — its toplevel output-enter/output-leave handlers are empty stubs — so
  `anythingFullscreen()` is one flat global count, while the shell runs one
  surface per output. The embed now disables WE's pause outright and the shell
  decides, per output. A shell that never sets `occluded` gets no fullscreen
  pausing at all; that is the deliberate default.

  Scope, so nobody sizes a power budget off it: `occluded` does not reach mpv. A
  video wallpaper keeps decoding at the file's frame rate, because only WE's own
  `setPause()` stops that and it is private to `WallpaperApplication`. What the
  flag drops is everything downstream of the decode — the blit, the fence, the
  publish, the repaint and the `wl_surface` commit — plus fifteen sixteenths of
  the render.

### Fixed
- Reverted `--fullscreen-pause-only-active` to `--no-fullscreen-pause`. The
  justification is **correctness, not cost**: WE's detector is output-blind (its
  toplevel output-enter/leave handlers are empty stubs), so `anythingFullscreen()`
  is one flat process-wide count while the shell runs one renderer per output — a
  game on one monitor froze the wallpapers on all the others, and no spelling of
  the flag fixes that, because the predicate is not where the bug is.

  The flag is also not a predicate knob: it decides whether a detector is
  constructed at all, and any spelling leaving `pauseOnFullscreen` set builds the
  real Wayland detector, which opens a second `wl_display_connect()` per wallpaper
  thread and does a `wl_display_roundtrip()` inside every `anythingFullscreen()` —
  once per frame, since `WallpaperApplication::render()` asks each time. That is
  recorded as a **secondary, unmeasured** consideration: Wayland message traffic is
  cheap in CPU terms and the per-frame round-trip has never been isolated as a
  measurable cost here. Do not cite it as the win.
- `failed` never cleared when a reload of the *same* project succeeded, and a
  late verdict could latch onto the next project. Verdicts now carry a load
  generation and disown themselves if it has moved on; `rendered` and `failed`
  are both retired on every rebuild (a `scaleMode` or `audioEnabled` change and a
  GL context loss included), and the two per-load latches are re-armed against
  the generation rather than against the rebuild — a `projectPath` that goes
  empty and comes straight back bumps twice without rebuilding anything, and used
  to leave `rendered` false for the rest of the session over a wallpaper that was
  rendering perfectly.
- A project switch kept the old scene-graph node alive after its GL texture had
  been deleted. The node holds a bare GL texture *name*; `releaseThread()` joins
  the WE thread, whose epilogue `glDeleteTextures` it inside the share group Qt
  is still drawing from, and the next `glGenTextures` hands the same name
  straight back — most likely to the incoming thread's own `targets[0]`, else to
  a Qt atlas or layer texture (the "widget's cached layer drawn fullscreen"
  symptom). The node is now dropped on every rebuild path, not only the
  context-change one.
- `glWaitSync` could name a fence the producer had already deleted, which is
  `GL_INVALID_VALUE` and *no wait at all* — a torn wallpaper frame with nothing
  in any log to explain it, because the render loop deliberately never reads the
  GL error queue after setup. The read and the wait are now one critical section.
- Release runs no longer throw away a 35-minute build. `out/` is uploaded as a
  workflow artifact before anything that can still fail (and the upload itself
  is `continue-on-error`, so a flaky artifact service cannot block the publish);
  every network call is wrapped in a bounded `ci-retry`; the release is created
  as a draft and flipped to published only after its assets are up, so a re-run
  converges instead of demanding a hand-deleted release; checkout runs before the
  dependency install; and the tag is validated against `package-we.sh`'s own
  character class in the first step rather than at minute 35.

## [0.2.1] — 2026-08-04

### Fixed
- A wallpaper that wedged inside WE's `setup()` could make a *later*, healthy
  wallpaper publish frames into freed memory. When the bounded join times out
  the thread is detached rather than freezing the shell — but `releaseThread()`
  then took that verdict from the object and destroyed it in the same statement.
  The detached thread is still inside `WeThread::run()`, a member function: it
  reads `mStop`, `mFps` and `mScaleMode`, locks `mMutex`, publishes into
  `mFrontTexture` and `mFrontFence`, notifies through `mOnFrame`, and WE holds
  `c_str()` pointers into its `std::string`s for the whole run. Freeing the
  object ran those destructors under a live reader and returned the address to
  the allocator — and the very next thing this class does is construct a
  replacement `WeThread` of exactly that size, which lands on the same address
  as often as not.

  The verdict is only knowable from inside the join, by which point the
  destructor has already committed to destroying what `run()` is using, so the
  join is hoisted into `[[nodiscard]] bool WeThread::stop()`. True means the
  thread finished and the object is quiescent; false means it was detached, and
  the caller leaks **both** the GL context and the `WeThread` on purpose. The
  earlier context leak protected the `EGLContext` and missed that the object
  holding the thread's state was freed alongside it. Costs a few hundred bytes
  and an OS thread per wedged wallpaper — the same bound the context leak
  already carried.

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

  **Superseded — see `occluded` under [Unreleased].** Two things above turned out
  to be wrong. `--fullscreen-pause-only-active` narrows the count to *activated*
  toplevels but the count is still global and still output-blind, so a game
  focused on one monitor went on freezing the wallpapers on the other two. And
  the shell *can* own the policy: `occluded` reaches the WE thread. The embed now
  passes `--no-fullscreen-pause` and the shell decides, per output.
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
