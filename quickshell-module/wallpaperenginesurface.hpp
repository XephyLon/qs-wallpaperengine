#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qquickframebufferobject.h>
#include <qtmetamacros.h>

namespace qs::wallpaperengine {

///! Renders a live Wallpaper Engine wallpaper into the scene graph.
/// WallpaperEngineSurface drives an embedded linux-wallpaperengine renderer into
/// a Qt-managed FBO (via QQuickFramebufferObject, so Qt owns the FBO/context and
/// the GL-state boundary) and displays that texture. Widgets can then frost
/// against it, the lock can blur it, transitions can run on it - all in-shell.
class WallpaperEngineSurface: public QQuickFramebufferObject {
	Q_OBJECT;
	QML_ELEMENT;
	// clang-format off
	/// Absolute path to the Wallpaper Engine project directory to render.
	Q_PROPERTY(QString projectPath READ projectPath WRITE setProjectPath NOTIFY projectPathChanged);
	/// Render continuously (true) or a single frame (false). Default true.
	Q_PROPERTY(bool live READ live WRITE setLive NOTIFY liveChanged);
	/// Target FPS while live. Default 60.
	Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY fpsChanged);
	// clang-format on

public:
	explicit WallpaperEngineSurface(QQuickItem* parent = nullptr);

	[[nodiscard]] Renderer* createRenderer() const override;

	[[nodiscard]] QString projectPath() const { return this->mProjectPath; }
	void setProjectPath(const QString& projectPath);

	[[nodiscard]] bool live() const { return this->mLive; }
	void setLive(bool live);

	[[nodiscard]] int fps() const { return this->mFps; }
	void setFps(int fps);

signals:
	void projectPathChanged();
	void liveChanged();
	void fpsChanged();

private:
	QString mProjectPath;
	bool mLive = true;
	int mFps = 60;
};

} // namespace qs::wallpaperengine
