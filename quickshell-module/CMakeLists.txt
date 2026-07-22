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
if (NOT DEFINED WALLPAPERENGINE_BUILD)
	set(WALLPAPERENGINE_BUILD "${WALLPAPERENGINE_SRC}/../build"
		CACHE PATH "linux-wallpaperengine build dir (lib + compile_commands.json)")
endif()
# Prefer the freshly built lib (it contains the CFbo driver + the registerDriver
# override patch); fall back to the installed one.
find_library(WALLPAPERENGINE_LIB
	NAMES linux-wallpaperengine-lib
	PATHS "${WALLPAPERENGINE_BUILD}/output" "${WALLPAPERENGINE_BUILD}" /opt/linux-wallpaperengine /usr/lib
	NO_DEFAULT_PATH
)
if (NOT WALLPAPERENGINE_LIB)
	find_library(WALLPAPERENGINE_LIB NAMES linux-wallpaperengine-lib
		PATHS "${WALLPAPERENGINE_BUILD}/output" "${WALLPAPERENGINE_BUILD}" /opt/linux-wallpaperengine /usr/lib)
endif()

qt_add_library(quickshell-wallpaperengine STATIC
	wallpaperenginesurface.cpp
	wethread.cpp
)

# WE headers pull in a large vendored-dep include set (glslang, SPIRV-Cross,
# json, kissfft, stb, quickjs, cef, generated protocols...). Rather than hardcode
# WE's build layout, harvest the exact -I/-isystem dirs from WE's
# compile_commands.json (the WE tree must be cmake-configured; bootstrap does it).
set(_we_incs "")
if (EXISTS "${WALLPAPERENGINE_BUILD}/compile_commands.json")
	execute_process(
		COMMAND python3 -c "import json,sys,shlex\ndb=json.load(open(sys.argv[1]))\ne=next(x for x in db if x['file'].endswith('CFboOpenGLDriver.cpp'))\nt=shlex.split(e['command']);o=[];i=0\nwhile i<len(t):\n    if t[i]=='-isystem': o.append(t[i+1]); i+=2; continue\n    if t[i].startswith('-I'): o.append(t[i][2:] or t[i+1]); i+=1; continue\n    i+=1\nprint(';'.join(o))" "${WALLPAPERENGINE_BUILD}/compile_commands.json"
		OUTPUT_VARIABLE _we_incs
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
endif()

target_include_directories(quickshell-wallpaperengine PRIVATE
	"${WALLPAPERENGINE_SRC}"
	${_we_incs}
)

target_link_libraries(quickshell-wallpaperengine PRIVATE
	Qt::Quick
	Qt::OpenGL
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
