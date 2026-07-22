# Quickshell module: Quickshell.WallpaperEngine
#
# Mirrors Quickshell's leaf-module pattern (see src/wayland/CMakeLists.txt):
# qt_add_library -> qt_add_qml_module -> qs_add_module_deps_light ->
# install_qml_module -> link the generated plugin into the `quickshell` target.
#
# Fast build path (no full WE rebuild): compile the FBO driver sources straight
# from the WE source tree into this module and link the INSTALLED
# liblinux-wallpaperengine-lib.so for the base-class symbols. ABI matches because
# we compile against the same source commit the installed lib was built from.

# --- linux-wallpaperengine location (set by bootstrap / -D) ------------------
# WALLPAPERENGINE_SRC: the WE source root that holds the (patched) headers +
#   CFbo* sources, i.e. <build>/linux-wallpaperengine/src
# WALLPAPERENGINE_LIB: the installed shared lib with the WE base symbols.
if (NOT DEFINED WALLPAPERENGINE_SRC)
	set(WALLPAPERENGINE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../linux-wallpaperengine/src"
		CACHE PATH "linux-wallpaperengine source root (headers + CFbo sources)")
endif()
find_library(WALLPAPERENGINE_LIB
	NAMES linux-wallpaperengine-lib
	PATHS /opt/linux-wallpaperengine /usr/lib
)

set(WE_DRV "${WALLPAPERENGINE_SRC}/WallpaperEngine/Render/Drivers")

qt_add_library(quickshell-wallpaperengine STATIC
	wallpaperenginesurface.cpp
	# FBO driver, compiled here (not via a WE rebuild):
	"${WE_DRV}/CFboOpenGLDriver.cpp"
	"${WE_DRV}/Output/CFboWindowOutput.cpp"
	"${WE_DRV}/Output/CFboOutputViewport.cpp"
)

# WE headers pull in glm + other vendored deps under the WE tree. Add the source
# root; extend with WE's other include dirs (glm, etc.) at build time as the
# transitive includes surface.
target_include_directories(quickshell-wallpaperengine PRIVATE
	"${WALLPAPERENGINE_SRC}"
)

target_link_libraries(quickshell-wallpaperengine PRIVATE
	Qt::Quick
	${WALLPAPERENGINE_LIB}
	EGL
	GLEW
	GL
)

qt_add_qml_module(quickshell-wallpaperengine
	URI Quickshell.WallpaperEngine
	VERSION 0.1
	DEPENDENCIES QtQuick
)

qs_add_module_deps_light(quickshell-wallpaperengine Quickshell)
install_qml_module(quickshell-wallpaperengine)
qs_module_pch(quickshell-wallpaperengine SET large)

target_link_libraries(quickshell PRIVATE quickshell-wallpaperengineplugin)
