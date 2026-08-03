# AGENTS.md

Reference for coding agents (and humans) working in this repository.

## What this is

`qs-wallpaperengine` builds a **patched Quickshell binary** that embeds a `Quickshell.WallpaperEngine`
QML module, so a Wallpaper Engine wallpaper renders *inside* the shell's own scene graph instead of
in a separate window underneath it.

It is a **satellite** of [`XephyLon/immaterial-impulse`](https://github.com/XephyLon/immaterial-impulse)
(the shell, referred to below as "the hub"). The hub pins which revision of this repo it builds, in
`sdata/subcmd-install/4.wallpaperengine.sh` as `WE_REF`. Nothing here reaches a user until that pin
moves — **a fix landing on `main` here ships to nobody.** That is not hypothetical: a fix once sat six
commits past the pin for four days while users hit the bug it fixed.

Three moving parts, and it matters which one you are in:

| layer | what it is | where |
|---|---|---|
| the module | our C++ — `WallpaperEngineSurface`, `WeThread` | `quickshell-module/` |
| Quickshell | upstream, lightly patched | fetched and built by `scripts/build-we.sh` |
| linux-wallpaperengine | vendored third-party engine, **not ours** | fetched; source readable under the build dir |

When something misbehaves, decide which layer owns it *before* you start reading code. Bugs here have
repeatedly been hunted in the wrong repo — a wallpaper that froze behind any fullscreen window was
chased through the shell's QML twice before the cause turned out to be linux-wallpaperengine pausing
itself.

## Building — read this before you type a build command

**A full build takes roughly 35 minutes** and saturates every core. Do not rebuild to "check
something". Reason from source; the vendored engine's source is readable without building it.

```bash
REPO_ROOT="$PWD" bash scripts/build-we.sh     # prints QS_BIN= and WE_LIB_DIR= on stdout
```

It builds into `$REPO_ROOT/build/`, which is **not** where an installed shell runs from — the hub
installs to `~/.cache/immaterial-impulse/qs-wallpaperengine-build/`. Building here therefore cannot
break a running desktop, which is the point.

To exercise a build without disturbing a running shell, use a standalone harness rather than
`launch-shell.sh` (which kills the user's shell and points at a stale config path):

```bash
export LD_LIBRARY_PATH="$PWD/build/linux-wallpaperengine/build/output:/opt/linux-wallpaperengine/lib:/opt/linux-wallpaperengine"
"$PWD/build/quickshell/build2/src/quickshell" -p test/vid.qml    # edit projectPath first
```

`test/` holds several such harnesses. A `FloatingWindow` with one `WallpaperEngineSurface` is enough
to prove a frame arrives, and costs seconds instead of a session.

## Architecture

`WallpaperEngineSurface` is a `QQuickItem`. `WeThread` runs linux-wallpaperengine on **its own thread
with its own EGL context**, created via `QOpenGLContext::setShareContext` against Qt's so the output
texture is usable by Qt. All of WE's GL stays off Qt's GUI and render threads — required on NVIDIA,
where WE's GL on Qt's threads corrupts Qt's EGL/Wayland dispatch.

Producer/consumer: `WeThread::run()` renders into one of two textures and publishes the other under a
mutex with a `glFenceSync`; `acquireTexture()` inserts a server-side wait so Qt never samples a
half-written frame.

### Invariants that are easy to get wrong

- **A share group shares objects, not state.** GL error state, bindings, viewport, current program are
  all per-context. One thread's GL failure cannot "poison" another context's *state*. What genuinely
  crosses is object names, driver VRAM, and a device-wide reset. Do not reach for context isolation to
  fix something that is actually a leak or a lifetime bug.
- **`glGetError` is not a usable failure signal here.** `INVALID_FRAMEBUFFER_OPERATION` is reported
  after mpv creates its texture on *every* video-wallpaper start, and the video then plays perfectly —
  linux-wallpaperengine never drains the error queue in its render path, so a stale error survives and
  gets misattributed. A `glGetError`-driven failure detector would black out every working video
  wallpaper. Failure detection is structural (`glCheckFramebufferStatus`, setup throwing, no driver)
  and must stay that way.
- **`mLive` only gates the repaint timer.** It does not stop `WeThread::run()`, which free-runs. It is
  not a power saving; it is what makes suppress/resume symmetrical for the consumer.
- **The detach path outlives its object.** A wallpaper wedged inside WE's `setup()` never observes the
  stop flag, so the join is bounded and the thread is detached rather than freezing the shell. That
  thread is still inside `WeThread::run()` — a *member* function using `mMutex`, `mFrontTexture`,
  `mFrontFence`, `mOnFrame` and the `std::string`s whose `c_str()` WE holds for its whole run. So
  `WeThread::stop()` returns a verdict, and on `false` `releaseThread()` leaks **both** the GL context
  and the `WeThread` object on purpose. Do not "tidy" either leak away: a replacement `WeThread` of
  exactly that size is constructed moments later and will land on a freed address as often as not,
  at which point the dead wallpaper publishes into the live one's state.

## Releasing

`docs/cutting-a-release.md` is authoritative. The short version:

1. Confirm `main` builds, and **actually run the binary** — a clean compile proves nothing about
   whether frames arrive.
2. Roll `CHANGELOG.md`, bump `VERSION`, `git tag vX.Y.Z && git push origin vX.Y.Z`.
3. CI packages and publishes the tarball + `manifest.json` + `SHA256SUMS`.
4. **Move the hub's `WE_REF` to the new tag.** Until you do, nothing ships.

Pin to a **tag**, not a SHA. A tag makes staleness legible; a bare SHA goes stale silently, and the
hub's installer only takes the prebuilt fast path for a `v*` ref — a SHA pin forces every user into a
35-minute source compile.

CI is fragile and has failed twice in one hour: once on `gh release create` hitting git's
"dubious ownership" in the container (fixed), once on a transient AUR RPC error at minute 18 of a
35-minute job with no retries. The publish step is last, so any late failure discards the whole build.
If you touch the workflow, check whether the expensive work is still thrown away on a network blip.

## Working on someone's live machine

- **Never `pkill -f <pattern>`** where the pattern could match your own command line. It matches the
  shell running it and kills your own session. This has happened.
- Do not restart the user's shell to test something here — use a standalone harness.
- Do not `git push` or open PRs unless asked.
