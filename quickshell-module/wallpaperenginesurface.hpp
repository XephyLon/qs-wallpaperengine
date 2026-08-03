#pragma once

#include <atomic>
#include <memory>

#include <qopenglcontext.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qquickitem.h>
#include <qtimer.h>
#include <qtmetamacros.h>

namespace qs::wallpaperengine {

class WeThread;

/// Wake-up channel from the WE thread to its surface.
///
/// Owned through a shared_ptr held by BOTH sides, because the two do not
/// necessarily die together: a wallpaper wedged inside WE's setup() gets its
/// thread DETACHED (see ~WeThread) and keeps running after the surface is gone.
/// A raw `this` in the thread's callback would then be dangling; a shared sink
/// is merely orphaned - it posts into an object with no receivers left, which is
/// a no-op.
class WeFrameSink: public QObject {
	Q_OBJECT;

public:
	// Called on the WE thread. Hops to the sink's own thread (the GUI thread,
	// where it is constructed) so the surface's update() happens there, not
	// inside WE's render loop.
	void post() {
		// One pending wake-up is enough. Without the coalesce, a GUI thread
		// stalled for a moment - a wallpaper switch tearing down mpv can do that -
		// comes back to a queue of `fps` identical wake-ups per stalled second.
		// Cleared before the emit, so a frame published mid-emit still posts.
		if (this->mPending.exchange(true)) return;
		QMetaObject::invokeMethod(
		    this,
		    [this] {
			    this->mPending.store(false);
			    emit this->frame();
		    },
		    Qt::QueuedConnection
		);
	}

signals:
	void frame();

private:
	std::atomic<bool> mPending {false};
};

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
	/// Target FPS while live. Default 60. This paces the wallpaper's frame
	/// PRODUCTION only; the surface repaints (and so the window commits) once per
	/// produced frame, so it is also the wallpaper's commit rate - it never
	/// repaints a frame that has not changed, and never misses one that has.
	Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY fpsChanged);
	/// Scaling mode: "fill" (crop to cover, default), "fit" (letterbox),
	/// "stretch" (distort to fill), or "default" (native, centered).
	Q_PROPERTY(QString scaleMode READ scaleMode WRITE setScaleMode NOTIFY scaleModeChanged);
	/// Play the wallpaper's audio (scene sounds / video soundtrack). Default
	/// false. Audio existence is decided when WE loads the project, so toggling
	/// this reloads the wallpaper (brief black-out, like a scaleMode change).
	Q_PROPERTY(bool audioEnabled READ audioEnabled WRITE setAudioEnabled NOTIFY audioEnabledChanged);
	/// True once the current project has produced its first rendered frame.
	/// Resets to false when projectPath changes. Lets QML start a wallpaper
	/// transition only when there is real content to show (not a black frame).
	Q_PROPERTY(bool rendered READ rendered NOTIFY renderedChanged);
	/// True when the current project cannot be rendered in the embed at all -
	/// the renderer failed to start, or its render targets came back INCOMPLETE
	/// (typically a scene whose source texture plus per-element composite
	/// buffers do not fit in VRAM at screen size). Resets to false when
	/// projectPath changes. While this is set the shell should fall back to the
	/// static wallpaper image, exactly as it already does for `web` projects -
	/// leaving the surface on screen only shows black.
	Q_PROPERTY(bool failed READ failed NOTIFY failedChanged);
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

	[[nodiscard]] bool audioEnabled() const { return this->mAudioEnabled; }
	void setAudioEnabled(bool audioEnabled);

	[[nodiscard]] bool rendered() const { return this->mRendered; }

	[[nodiscard]] bool failed() const { return this->mFailed; }

signals:
	void projectPathChanged();
	void liveChanged();
	void fpsChanged();
	void scaleModeChanged();
	void audioEnabledChanged();
	void renderedChanged();
	void failedChanged();

protected:
	QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

private:
	QString mProjectPath;
	bool mLive = true;
	int mFps = 60;
	QString mScaleMode = QStringLiteral("fill");
	bool mAudioEnabled = false;
	bool mRendered = false;      // first frame of the current project seen (GUI thread)
	bool mFailed = false;        // current project cannot render (GUI thread)
	bool mLoadFrameSeen = false; // per-load latch (render thread only)
	bool mFailSeen = false;      // per-load latch (render thread only)

	// Declared before mThread so it outlives it: the thread uses this context's
	// native EGLContext and must be joined before the context is destroyed.
	std::unique_ptr<QOpenGLContext> mShareContext;
	std::unique_ptr<WeThread> mThread;
	QString mLoadedPath;
	// The share TARGET the WE context was built against: the process-global
	// share context when AA_ShareOpenGLContexts is set (then it lives for the
	// app's lifetime and scene-graph context recreations don't matter), else
	// the scene-graph context itself. Only an identity token (never
	// dereferenced). nullptr until the first build.
	QOpenGLContext* mLoadedContext = nullptr;
	// Pointer identity alone misses a destroy+recreate that reuses the same heap
	// address (common: the render thread frees and reallocates back to back), and
	// then WE's texture NAMES silently alias unrelated textures in the new share
	// group (the wallpaper draws e.g. a widget's cached layer, fullscreen).
	// aboutToBeDestroyed on the adopted context latches this flag instead; set and
	// read on the render thread.
	bool mContextLost = false;

	// Repaints are driven by the WE thread publishing a frame, through this sink.
	// Handed to WeThread as a shared_ptr copy inside its callback, so it outlives
	// a detached thread's last notification.
	std::shared_ptr<WeFrameSink> mFrameSink;
	// Set by the sink, cleared by the stall watchdog: "the producer published at
	// least once since the last tick".
	bool mFrameSincePoll = false;
	// NOT a repaint clock. It only fires update() when the producer has gone
	// silent for a whole interval, so that updatePaintNode still runs at a slow
	// idle rate to notice a lost GL context or a wallpaper that died without
	// getting as far as latching `failed`. A producing wallpaper never lets it
	// through, so it costs nothing in the normal case.
	QTimer mStallPoll;

	void updateFrameDriver();
	// Tear down mThread and mShareContext - unless the WE thread had to be
	// detached, in which case BOTH are leaked deliberately: it is still running
	// inside WeThread::run(), on that object, with that context current.
	void releaseThread();
};

} // namespace qs::wallpaperengine
