#pragma once

#include <memory>

#include <qopenglcontext.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qquickitem.h>
#include <qtimer.h>
#include <qtmetamacros.h>

namespace qs::wallpaperengine {

class WeThread;

///! Renders a live Wallpaper Engine wallpaper into the scene graph.
/// WallpaperEngineSurface runs an embedded linux-wallpaperengine renderer on its
/// own thread + EGL context (sharing GL objects with Qt's) and displays the
/// resulting texture as a scene-graph node. Keeping all of WE's GL/EGL off Qt's
/// threads avoids corrupting Qt's Wayland/EGL dispatch (fatal on NVIDIA).
/// Widgets can then frost against it, the lock can blur it, transitions can run
/// on it - all in-shell.
class WallpaperEngineSurface: public QQuickItem {
	Q_OBJECT;
	QML_ELEMENT;
	// clang-format off
	/// Absolute path to the Wallpaper Engine project directory to render.
	Q_PROPERTY(QString projectPath READ projectPath WRITE setProjectPath NOTIFY projectPathChanged);
	/// Render continuously (true) or a single frame (false). Default true.
	Q_PROPERTY(bool live READ live WRITE setLive NOTIFY liveChanged);
	/// Target FPS while live. Default 60.
	Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY fpsChanged);
	/// Scaling mode: "fill" (crop to cover, default), "fit" (letterbox),
	/// "stretch" (distort to fill), or "default" (native, centered).
	Q_PROPERTY(QString scaleMode READ scaleMode WRITE setScaleMode NOTIFY scaleModeChanged);
	/// True once the current project has produced its first rendered frame.
	/// Resets to false when projectPath changes. Lets QML start a wallpaper
	/// transition only when there is real content to show (not a black frame).
	Q_PROPERTY(bool rendered READ rendered NOTIFY renderedChanged);
	// clang-format on

public:
	explicit WallpaperEngineSurface(QQuickItem* parent = nullptr);
	~WallpaperEngineSurface() override;
	Q_DISABLE_COPY_MOVE(WallpaperEngineSurface);

	[[nodiscard]] QString projectPath() const { return this->mProjectPath; }
	void setProjectPath(const QString& projectPath);

	[[nodiscard]] bool live() const { return this->mLive; }
	void setLive(bool live);

	[[nodiscard]] int fps() const { return this->mFps; }
	void setFps(int fps);

	[[nodiscard]] QString scaleMode() const { return this->mScaleMode; }
	void setScaleMode(const QString& scaleMode);

	[[nodiscard]] bool rendered() const { return this->mRendered; }

signals:
	void projectPathChanged();
	void liveChanged();
	void fpsChanged();
	void scaleModeChanged();
	void renderedChanged();

protected:
	QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

private:
	QString mProjectPath;
	bool mLive = true;
	int mFps = 60;
	QString mScaleMode = QStringLiteral("fill");
	bool mRendered = false;      // first frame of the current project seen (GUI thread)
	bool mLoadFrameSeen = false; // per-load latch (render thread only)

	// Declared before mThread so it outlives it: the thread uses this context's
	// native EGLContext and must be joined before the context is destroyed.
	std::unique_ptr<QOpenGLContext> mShareContext;
	std::unique_ptr<WeThread> mThread;
	QString mLoadedPath;
	// The Qt GL context the share context + WE thread were built against. Only an
	// identity token (never dereferenced): if Qt tears down and recreates the
	// scene-graph context - which Hyprland's fullscreen direct-scanout can force -
	// the old shared EGLContext orphans and WE's texture stops being valid, so we
	// must rebuild against the new context. nullptr until the first build.
	QOpenGLContext* mLoadedContext = nullptr;
	QTimer mRepaint; // GUI-thread repaint driver at mFps

	void updateRepaintTimer();
};

} // namespace qs::wallpaperengine
