# Quickshell module: Quickshell.WallpaperEngine
#
# Drop this directory into the Quickshell source tree as src/wallpaperengine/
# and add it from src/CMakeLists.txt (see INTEGRATION.md). Quickshell defines its
# own module helper macro (grep the real tree for `qs_module` / `qs_pch` /
# `qt_add_qml_module` in another leaf module, e.g. src/wayland/CMakeLists.txt,
# and mirror it exactly). The block below is the Qt-native equivalent as a
# reference; reconcile the macro names with the pinned tree at build time.

# --- Reference (align with Quickshell's actual macro) ------------------------
qt_add_library(quickshell-wallpaperengine STATIC
	wallpaperenginesurface.cpp
)

qt_add_qml_module(quickshell-wallpaperengine
	URI Quickshell.WallpaperEngine
	VERSION 0.1
	DEPENDENCIES QtQuick
)

target_link_libraries(quickshell-wallpaperengine PRIVATE
	Qt6::Quick
	Qt6::Qml
)

# linux-wallpaperengine, built from source with the FBO output driver
# (../we-fbo-driver). Points at the installed lib + its headers; the from-source
# build installs headers the packaged -git currently does not.
find_library(WALLPAPERENGINE_LIB
	NAMES linux-wallpaperengine-lib
	PATHS /opt/linux-wallpaperengine /usr/lib
)
target_include_directories(quickshell-wallpaperengine PRIVATE
	${WALLPAPERENGINE_INCLUDE_DIR}   # set by bootstrap to the WE source include/
)
target_link_libraries(quickshell-wallpaperengine PRIVATE
	${WALLPAPERENGINE_LIB}
	EGL
	GLESv2
)
# -----------------------------------------------------------------------------
