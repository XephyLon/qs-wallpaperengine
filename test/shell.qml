import QtQuick
import Qt5Compat.GraphicalEffects
import Quickshell
import Quickshell.WallpaperEngine

ShellRoot {
    FloatingWindow {
        implicitWidth: 1200
        implicitHeight: 400
        color: "#161616"
        Row {
            anchors.centerIn: parent
            spacing: 12
            WallpaperEngineSurface {
                id: we
                width: 580; height: 340
                projectPath: "/home/xephy/.local/share/Steam/steamapps/workshop/content/431960/3008040633"
                live: true; fps: 60
            }
            Item {
                width: 580; height: 340
                WallpaperEngineSurface {
                    anchors.fill: parent
                    projectPath: we.projectPath
                    live: true
                    layer.enabled: true
                    layer.effect: FastBlur { radius: 64 }
                }
            }
        }
    }
}
