# Wiring `Quickshell.WallpaperEngine` into the Quickshell build

Target tree: upstream Quickshell @ `7511545ee20664e3b8b8d3322c0ffe7567c56f7a`
(what `illogical-impulse-quickshell-git` pins).

## 1. Place the module

    cp -r quickshell-module <quickshell>/src/wallpaperengine

## 2. Register it in `src/CMakeLists.txt`

Add alongside the other leaf modules (search the file for how `wayland` /
`services` are added — Quickshell gates modules behind options and its own
`qs_module` macro; match that form rather than the raw `qt_add_qml_module` in the
reference CMakeLists):

    add_subdirectory(wallpaperengine)

and add the resulting target to whatever aggregates the module objects/plugins
(again, mirror an existing leaf module — the exact variable name lives in the
pinned tree, e.g. `target_link_libraries(quickshell PRIVATE quickshell-wallpaperengineplugin)`).

## 3. Reconcile the module macro

Quickshell wraps `qt_add_qml_module` in a helper (`qs_module` or similar) that
also handles the `_p.hpp`/moc/`_autogen` layout you can see in the installed
build dir:

    /usr/src/debug/illogical-impulse-quickshell-git/quickshell/build/src/<mod>/quickshell-<mod>plugin_autogen/

Grep the pinned source for `macro(qs_module` / `function(qs_module` (top-level
`cmake/` or `src/CMakeLists.txt`) and replace the reference block in
`CMakeLists.txt` with a call to it. The module URI must be
`Quickshell.WallpaperEngine` and export `WallpaperEngineSurface`.

## 4. linux-wallpaperengine link

`bootstrap.sh` sets `WALLPAPERENGINE_INCLUDE_DIR` to the WE source `include/`
(the from-source build, patched with `../we-fbo-driver`, exposes the
`WallpaperEngine::Render::*` headers the packaged `-git` omits) and ensures
`liblinux-wallpaperengine-lib.so` is on the link path.

## 5. Use from QML (shell repo, branch `feat/embed-wallpaperengine`)

    import Quickshell.WallpaperEngine

    WallpaperEngineSurface {
        anchors.fill: parent
        projectPath: Config.options.wallpaperSelector.wallpaperEngine.activePath
        live: !GlobalStates.screenLocked   // freeze on lock if desired
        fps: Config.options.wallpaperSelector.wallpaperEngine.fps
    }

Then a `ShaderEffectSource { sourceItem: <that surface> }` feeds the widget
frost / lock blur / transition shaders — the same texture, in-shell.

## 6. Pause a covered output (`occluded`)

The embed passes `--no-fullscreen-pause`, so linux-wallpaperengine's own
fullscreen pause is off. It has to be: its detector has no concept of an output
and counts every fullscreen toplevel through one flat counter, and the shell
runs one surface — therefore one detector — per output, so a fullscreen window
on one monitor froze the wallpapers on all the others. (It also opens a second
Wayland connection per output and does a blocking round-trip per frame, from
inside frame production.)

The shell knows which output is covered, so the shell owns the policy:

    WallpaperEngineSurface {
        // ...
        occluded: thisOutputIsCoveredByAFullscreenWindow   // per output, not global
    }

While `occluded` is true the renderer idles at a few frames a second and stops
publishing, so the surface stops repainting and the window stops committing.
It is a live toggle — unlike `scaleMode` / `audioEnabled` it does not reload the
wallpaper — and clearing it produces a fresh frame within a frame or two.

Two things to know:

- `occluded` is not `live`. `live: false` freezes the surface but leaves the
  renderer running at full rate (it means "stop showing me new frames", e.g. on
  lock). `occluded` reaches the renderer thread, which is the only way to stop a
  video wallpaper rendering at full rate.
- It does **not** reach mpv. A video wallpaper keeps decoding at the file's frame
  rate while `occluded` is set — only linux-wallpaperengine's own `setPause()`
  stops that, and it is private to `WallpaperApplication` and unreachable from
  the embed. What you save is the blit, the fence, the publish, the repaint, the
  commit and fifteen sixteenths of the render. Size a power budget off that, not
  off "the wallpaper is paused".
- A surface that is occluded from the moment its project loads does not report
  `rendered` until it is uncovered — nothing has been drawn yet. Gate wallpaper
  transitions on outputs the user can actually see.

A shell that never sets `occluded` gets no fullscreen pausing at all: the
wallpaper renders at `fps` for as long as `live` is true, whatever is on top of
it. That is the deliberate default — the pause is opt-in, per output.
