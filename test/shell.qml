import QtQuick
import Qt5Compat.GraphicalEffects
import Quickshell
import Quickshell.WallpaperEngine

// Minimal smoke test for the embedded Wallpaper Engine surface. Left: the live
// WE frames pulled into Quickshell's scene graph. Right: an in-shell FastBlur of
// that same texture - the whole point (frost with no compositor/xray tricks).
ShellRoot {
    FloatingWindow {
        implicitWidth: 1200
        implicitHeight: 360
        color: "#161616"

        Row {
            anchors.fill: parent
            anchors.margins: 8
            spacing: 10

            WallpaperEngineSurface {
                id: we
                width: 580
                height: 340
                // A real WE project directory (scene or video). Override as needed.
                projectPath: "/home/xephy/.local/share/Steam/steamapps/workshop/content/431960/3008040633"
                live: true
                fps: 60
            }

            Item {
                width: 580
                height: 340
                WallpaperEngineSurface {
                    id: we2
                    anchors.fill: parent
                    projectPath: we.projectPath
                    live: true
                    layer.enabled: true
                    layer.effect: FastBlur { radius: 64 }
                }
            }
        }

        Text {
            anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter }
            color: "white"
            text: "hasContent=" + we.hasContent + "  size=" + we.sourceSize.width + "x" + we.sourceSize.height
        }
    }
}
