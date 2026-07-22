#pragma once

#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qquickitem.h>
#include <qsgnode.h>
#include <qtmetamacros.h>

namespace qs::wallpaperengine {

class WallpaperEngineContext; // owns the WE renderer + shared FBO (see .cpp)

///! Renders a live Wallpaper Engine wallpaper into the scene graph.
/// WallpaperEngineSurface drives an embedded linux-wallpaperengine renderer into
/// an offscreen FBO on Quickshell's own GL context, and displays that texture as
/// an ordinary QQuickItem. Because the frames live in the shell's scene graph,
/// widgets can frost against it (ShaderEffectSource), the lock can blur it, and
/// wallpaper transitions can run shaders on it - all in-shell, no compositor
/// tricks.
///
/// Modeled on Quickshell's ScreencopyView (src/wayland/screencopy/view.hpp).
class WallpaperEngineSurface: public QQuickItem {
	Q_OBJECT;
	QML_ELEMENT;
	// clang-format off
	/// Absolute path to the Wallpaper Engine project directory to render
	/// (the shell writes this to wallpaperSelector.wallpaperEngine.activePath).
	/// Empty clears the surface.
	Q_PROPERTY(QString projectPath READ projectPath WRITE setProjectPath NOTIFY projectPathChanged);
	/// If true, renders continuously at @@fps. If false, renders a single frame.
	Q_PROPERTY(bool live READ live WRITE setLive NOTIFY liveChanged);
	/// Target frames per second while @@live. Defaults to 60.
	Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY fpsChanged);
	/// True once the first frame has been rendered and the texture is valid.
	Q_PROPERTY(bool hasContent READ default NOTIFY hasContentChanged BINDABLE bindableHasContent);
	/// Native pixel size of the rendered wallpaper. Valid when @@hasContent.
	Q_PROPERTY(QSize sourceSize READ default NOTIFY sourceSizeChanged BINDABLE bindableSourceSize);
	// clang-format on

public:
	explicit WallpaperEngineSurface(QQuickItem* parent = nullptr);
	~WallpaperEngineSurface() override;
	Q_DISABLE_COPY_MOVE(WallpaperEngineSurface);

	void componentComplete() override;

	[[nodiscard]] QString projectPath() const { return this->mProjectPath; }
	void setProjectPath(const QString& projectPath);

	[[nodiscard]] bool live() const { return this->mLive; }
	void setLive(bool live);

	[[nodiscard]] int fps() const { return this->mFps; }
	void setFps(int fps);

	[[nodiscard]] QBindable<bool> bindableHasContent() { return &this->bHasContent; }
	[[nodiscard]] QBindable<QSize> bindableSourceSize() { return &this->bSourceSize; }

signals:
	void projectPathChanged();
	void liveChanged();
	void fpsChanged();
	void hasContentChanged();
	void sourceSizeChanged();
	/// The embedded renderer failed to start or died. The surface shows nothing.
	void stopped();

protected:
	QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

private slots:
	// Advances the WE render one frame on the scene-graph render thread, before
	// Qt renders, so the FBO texture is fresh for updatePaintNode.
	void onBeforeRendering();

private:
	void createContext(); // lazily builds WE renderer on the window's GL context
	void destroyContext();
	void reload();        // (re)loads mProjectPath into the context

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY(WallpaperEngineSurface, bool, bHasContent, &WallpaperEngineSurface::hasContentChanged);
	Q_OBJECT_BINDABLE_PROPERTY(WallpaperEngineSurface, QSize, bSourceSize, &WallpaperEngineSurface::sourceSizeChanged);
	// clang-format on

	QString mProjectPath;
	bool mLive = true;
	int mFps = 60;
	bool completed = false;
	WallpaperEngineContext* context = nullptr;
};

} // namespace qs::wallpaperengine
