#pragma once

#include <atomic>
#include <cstdint>
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
/// is merely orphaned - once the surface is gone it posts into an object with no
/// receivers left, which is a no-op.
///
/// The same sink is deliberately handed to every WeThread the surface builds, so
/// on a project switch a detached thread's last wake-up does reach the LIVE
/// surface. That is bounded to one or two spurious repaints and costs nothing but
/// them - see the note on the shared sink in wethread.hpp.
///
/// Which side drops the last reference is therefore not fixed, and a detached
/// thread dropping it would run ~QObject on a thread that does not own this
/// object. The shared_ptr is built with a deleter that checks affinity and falls
/// back to deleteLater() for exactly that case - see the constructor.
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
	/// Show new frames (true) or hold the one already on screen (false). Default
	/// true. This is a CONSUMER-side switch and nothing more: clearing it stops
	/// the surface repainting, and so stops the window committing, but the
	/// renderer thread goes on producing at `fps` behind the frozen image. It is
	/// what you want for a lock screen or a screenshot; it is NOT a way to make an
	/// invisible wallpaper cheap - `occluded` below is the property that idles the
	/// renderer itself.
	Q_PROPERTY(bool live READ live WRITE setLive NOTIFY liveChanged);
	/// Target frame rate for the renderer. Default 60. Independent of `live`,
	/// which only decides whether produced frames are shown; `occluded` is the one
	/// property that overrides this rate, dropping production to a few hertz for
	/// as long as the output is covered. This paces the wallpaper's frame
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
	/// Set by the shell when a fullscreen window covers THIS output. Default
	/// false. While it is set the wallpaper's renderer idles at a few frames a
	/// second and publishes nothing, so the surface stops repainting and the
	/// window stops committing; clearing it produces a fresh frame within a frame
	/// or two. Unlike `live`, this reaches the renderer thread, so it is the only
	/// thing that throttles the renderer at all. It is a live toggle - it does not
	/// reload the wallpaper the way scaleMode and audioEnabled do.
	///
	/// It does not reach mpv. A video wallpaper goes on decoding at the file's
	/// frame rate while this is set, because the only thing that stops that is the
	/// renderer's own setPause(), which is private to its application class and
	/// unreachable from the embed. What this does drop is everything downstream of
	/// the decode - the blit, the fence, the publish, the surface's repaint and the
	/// window's commit - plus fifteen sixteenths of the render itself. Size a power
	/// budget off that, not off "the wallpaper is paused".
	///
	/// The embedded renderer's own fullscreen pause is disabled, because its
	/// detector has no concept of an output and counts every fullscreen window
	/// through one global counter, while the shell runs one surface per output -
	/// a game on one monitor froze the wallpapers on all the others. The shell
	/// knows which output is covered, so the shell owns the policy: a shell that
	/// never sets this gets no fullscreen pausing at all.
	Q_PROPERTY(bool occluded READ occluded WRITE setOccluded NOTIFY occludedChanged);
	/// True once the wallpaper currently on screen has produced a frame. Resets to
	/// false whenever that stops being true of what is on screen: a projectPath
	/// change, and equally a reload of the *same* project - a scaleMode or
	/// audioEnabled change, or a lost GL context - because a reload drops the
	/// scene-graph node holding the old renderer's texture, and there is nothing
	/// on screen again until the new renderer publishes. Lets QML start a
	/// wallpaper transition only when there is real content to show (not a black
	/// frame).
	Q_PROPERTY(bool rendered READ rendered NOTIFY renderedChanged);
	/// True when the current project cannot be rendered in the embed at all -
	/// the renderer failed to start, or its render targets came back INCOMPLETE
	/// (typically a scene whose source texture plus per-element composite
	/// buffers do not fit in VRAM at screen size). Resets to false on every
	/// reload, not only on a projectPath change: a scaleMode or audioEnabled
	/// change and a GL context loss all restart the renderer, and the failure may
	/// well have been transient (a VRAM shortfall clears when whatever else was
	/// holding the memory lets go), so every attempt gets its own verdict. While
	/// this is set the shell should fall back to the static wallpaper image,
	/// exactly as it already does for `web` projects - leaving the surface on
	/// screen only shows black.
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

	[[nodiscard]] bool occluded() const { return this->mOccluded; }
	void setOccluded(bool occluded);

	[[nodiscard]] bool rendered() const { return this->mRendered; }

	[[nodiscard]] bool failed() const { return this->mFailed; }

signals:
	void projectPathChanged();
	void liveChanged();
	void fpsChanged();
	void scaleModeChanged();
	void audioEnabledChanged();
	void occludedChanged();
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
	bool mOccluded = false;      // a fullscreen window covers this output (GUI thread)
	bool mRendered = false;      // first frame of the current load seen (GUI thread)
	bool mFailed = false;        // current load cannot render (GUI thread)
	bool mLoadFrameSeen = false; // per-load latch (render thread only)
	bool mFailSeen = false;      // per-load latch (render thread only)
	// Which load the two latches above are armed for (render thread only). They
	// used to be re-armed by the rebuild block in updatePaintNode, which is one
	// bump short: see the re-arm comment there.
	std::uint64_t mAckedGeneration = 0;

	// Which load the render thread's queued property posts belong to. A post is
	// made from updatePaintNode with the GUI thread blocked for the scene-graph
	// sync, so it lands BEHIND everything already queued there - including a
	// projectPath write that has not run yet. Without an identity on the post,
	// project A's "it failed" verdict arrives after setProjectPath(B) has cleared
	// the flag and marks B failed before B ever loaded. Each post captures this
	// counter by value and drops itself if it no longer matches on arrival. A
	// comparison against mProjectPath cannot do the job in its place: a scaleMode
	// or audioEnabled reload keeps the same path.
	//
	// Bumped on the render thread whenever the WE thread is rebuilt, and on the
	// GUI thread by every setter that invalidates the running load - atomic
	// because those are different threads, even though the scene-graph sync
	// happens to serialise them today. Every bump re-arms mLoadFrameSeen/mFailSeen
	// on the next pass through updatePaintNode - against mAckedGeneration, NOT
	// against the rebuild, because a projectPath that goes empty and comes back
	// bumps twice without ever rebuilding anything - so a dropped post is always
	// re-made for the load that superseded it and neither property can get stuck.
	std::atomic<std::uint64_t> mLoadGeneration {0};

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
	// a detached thread's last notification. The callback lives in the WeThread's
	// mOnFrame, and on the detach path releaseThread() LEAKS that object rather
	// than freeing it, so the reference genuinely survives and the sink's
	// refcount is non-zero for as long as the thread can still post through it.
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
