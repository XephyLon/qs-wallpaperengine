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
