// glew before any Qt/GL header.
#include <GL/glew.h>

#include "wallpaperenginesurface.hpp"
#include "wethread.hpp"

#include <EGL/egl.h>

#include <string>

#include <qcoreapplication.h>
#include <qopenglcontext.h>
#include <qopenglcontext_platform.h>
#include <qquickwindow.h>
#include <qsgsimpletexturenode.h>
#include <qsgtexture.h>
#include <qsgtexture_platform.h>

namespace qs::wallpaperengine {

namespace {

std::string assetsDir() {
	return std::string(qgetenv("HOME").constData())
	    + "/.local/share/Steam/steamapps/common/wallpaper_engine/assets";
}

} // namespace

WallpaperEngineSurface::WallpaperEngineSurface(QQuickItem* parent): QQuickItem(parent) {
	this->setFlag(QQuickItem::ItemHasContents);
	// Repaint when the WE thread has actually produced a frame - not on a clock
	// of our own running at `fps`. Two clocks at nominally the same rate drift,
	// so a poll clock both repeats frames (a full-screen repaint, a full-surface
	// wl_surface commit and a compositor recomposite for a texture that has not
	// changed) and drops fresh ones. It also is not even the right rate:
	// 1000/fps is integer milliseconds, so fps=24 polled at 41ms is 24.4Hz,
	// fps=144 polls at 166Hz and fps=90 at 90.9Hz.
	//
	// The sink hop puts update() on the GUI thread rather than calling it from
	// inside WE's render loop or from updatePaintNode on the render thread, which
	// keeps the scene-graph sync race-free exactly as the old timer did.
	this->mFrameSink = std::make_shared<WeFrameSink>();
	QObject::connect(this->mFrameSink.get(), &WeFrameSink::frame, this, [this] {
		this->mFrameSincePoll = true;
		if (this->mLive && !this->mProjectPath.isEmpty()) this->update();
	});
	QObject::connect(&this->mStallPoll, &QTimer::timeout, this, [this] {
		if (this->mFrameSincePoll) {
			this->mFrameSincePoll = false;
			return;
		}
		this->update();
	});
	this->mStallPoll.setInterval(1000);
	this->updateFrameDriver();

	// Stop + join the WE thread while the event loop and render thread are still
	// alive. If we wait for the item destructor (Qt teardown), the scene-graph
	// render thread deadlocks against the still-running WE thread and the process
	// hangs on quit (SIGTERM appears ignored). aboutToQuit fires first, on the
	// GUI thread, so the join happens at a safe point.
	QObject::connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { this->releaseThread(); });
}

void WallpaperEngineSurface::releaseThread() {
	if (!this->mThread) {
		this->mShareContext.reset();
		return;
	}

	// stop() bounds its join: a wallpaper wedged inside WE's setup() never
	// observes the stop flag, so after a few seconds the thread is DETACHED and
	// left running rather than freezing the shell. Detached does not mean gone,
	// and TWO things it is still using have to outlive this call.
	//
	// The EGLContext: it is still current on that thread, which still issues GL
	// through it. Destroying this QOpenGLContext would pull it out from under a
	// live thread mid-draw, in a context that shares its object namespace with
	// Qt's. That is a real mechanism for one bad wallpaper to damage every later
	// one.
	//
	// The WeThread object: the detached thread is inside WeThread::run(), a
	// MEMBER function that reads mStop/mFps/mScaleMode, locks mMutex, publishes
	// into mFrontTexture/mFrontFence and notifies through mOnFrame. Destroying
	// it would run those members' destructors under a live reader, and freeing
	// it would hand the address back to the allocator - and the very next thing
	// this class does is construct a REPLACEMENT WeThread of exactly that size,
	// which lands on that address as often as not. The dead wallpaper would then
	// publish its texture and its fence into the live one's state.
	//
	// So on the detach path, leak both, deliberately. That thread has already
	// stranded an OS thread, its mpv/SDL state and WE's own GL objects; a few
	// hundred more bytes, once per wedged wallpaper, is the cheap half of the
	// trade. The alternative is undefined behaviour in the shared group.
	if (!this->mThread->stop()) {
		qWarning(
		    "WallpaperEngineSurface: WE thread detached; leaking its GL context and thread "
		    "object deliberately"
		);
		(void) this->mThread.release();
		(void) this->mShareContext.release();
		return;
	}

	this->mThread.reset();
	this->mShareContext.reset();
}

void WallpaperEngineSurface::updateFrameDriver() {
	// live=false freezes on the current frame: the producer keeps running (WE has
	// no pause that survives a resume cleanly) but its notifications stop
	// repainting us, so the surface holds the last texture it published and the
	// window stops committing entirely.
	if (this->mLive && !this->mProjectPath.isEmpty()) {
		this->mFrameSincePoll = false;
		if (!this->mStallPoll.isActive()) this->mStallPoll.start();
	} else {
		this->mStallPoll.stop();
	}
}

// aboutToQuit normally gets here first, but an item destroyed on its own (the
// Loader going inactive, a monitor disappearing) still has to go through
// releaseThread rather than the members' declaration order, so that a detached
// thread's context is leaked rather than destroyed under it.
WallpaperEngineSurface::~WallpaperEngineSurface() { this->releaseThread(); }

QSGNode* WallpaperEngineSurface::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* /*data*/) {
	auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);

	const int w = static_cast<int>(this->width());
	const int h = static_cast<int>(this->height());
	if (w <= 0 || h <= 0 || this->mProjectPath.isEmpty()) {
		delete node;
		return nullptr;
	}

	// Runs on the render thread with Qt's GL context current. Build a share
	// context via Qt (so its config + robustness flags match Qt's - raw
	// eglCreateContext sharing fails EGL_BAD_MATCH on NVIDIA), then hand its
	// native EGLContext to the WE thread for a surfaceless makeCurrent.
	//
	// Share against the process-global share context when there is one
	// (AA_ShareOpenGLContexts, set by the patched main.cpp): every scene-graph
	// context Qt creates joins that share group, so WE's textures stay valid
	// across the compositor-forced scene-graph context recreations Hyprland's
	// fullscreen direct-scanout causes - no rebuild, no black-out. A game
	// session can force several recreations a minute; rebuilding the WE thread
	// for each one is what used to wedge video wallpapers into permanent black
	// (mpv teardown under GPU contention overran the join deadline, the old
	// thread detached, and two WE instances then fought over process globals).
	//
	// Without a global share context (stock build), fall back to sharing with
	// the current SG context and rebuilding when it dies. Pointer inequality
	// alone is not enough for that: a destroy+recreate can hand the new
	// QOpenGLContext the old one's heap address, so mContextLost (latched by
	// aboutToBeDestroyed on the adopted context) is the authoritative signal.
	auto* qtCtx = QOpenGLContext::currentContext();
	auto* shareTarget = QOpenGLContext::globalShareContext();
	if (!shareTarget) shareTarget = qtCtx;
	const bool contextChanged =
	    this->mThread && (this->mLoadedContext != shareTarget || this->mContextLost);
	if (contextChanged) {
		qInfo("WallpaperEngineSurface: GL context changed; rebuilding WE thread");
		// The old node's texture wraps a GL texture NAME from the destroyed
		// context's share group. In the new share group that name is dangling -
		// or recycled for unrelated live textures (a widget's cached layer,
		// drawn fullscreen) - so keeping the node on screen while WE rebuilds
		// draws garbage. Drop it: a brief black-out until the first new frame
		// is the correct degradation.
		delete node;
		node = nullptr;
	}
	if (!this->mThread || this->mLoadedPath != this->mProjectPath || contextChanged) {
		this->releaseThread();

		auto share = std::make_unique<QOpenGLContext>();
		share->setFormat(qtCtx->format());
		share->setShareContext(shareTarget);
		if (!share->create() || !share->shareContext()) {
			qWarning("WallpaperEngineSurface: failed to create shared GL context");
			return node;
		}

		auto* eglCtx = share->nativeInterface<QNativeInterface::QEGLContext>();
		auto dpy = eglGetCurrentDisplay();
		if (!eglCtx || dpy == EGL_NO_DISPLAY) {
			qWarning("WallpaperEngineSurface: no EGL context handle available");
			return node;
		}

		// Adopting a (possibly new) share target: latch its destruction so the
		// next updatePaintNode rebuilds even if a replacement reuses this heap
		// address. The global share context lives until app shutdown, so with
		// it this effectively never fires; the latch matters for the
		// fallback-to-SG-context case. aboutToBeDestroyed fires on the
		// context's owning thread, so a plain bool is safe for the SG-context
		// case (render thread, same as this code). Reconnecting on a
		// same-context project switch would stack duplicate connections, so
		// only connect when the target is actually new.
		if (this->mLoadedContext != shareTarget || this->mContextLost) {
			QObject::connect(shareTarget, &QOpenGLContext::aboutToBeDestroyed, this, [this] {
				this->mContextLost = true;
			}, Qt::DirectConnection);
		}
		this->mContextLost = false;
		this->mShareContext = std::move(share);
		this->mLoadedPath = this->mProjectPath;
		this->mLoadedContext = shareTarget;
		this->mLoadFrameSeen = false; // new project: re-arm the first-frame latch
		this->mFailSeen = false;      // ... and the failure latch
		// The callback holds its own shared_ptr to the sink, so the sink survives
		// for as long as the thread can still call it - including a thread that
		// outlived this item by being detached.
		this->mThread = std::make_unique<WeThread>(
		    dpy, eglCtx->nativeContext(), this->mProjectPath.toStdString(), assetsDir(), w, h,
		    this->mFps, this->mScaleMode.toStdString(), this->mAudioEnabled,
		    [sink = this->mFrameSink] { sink->post(); }
		);
	}

	// The WE thread gave up on this project. Report it once, on the GUI thread,
	// so QML can put the static wallpaper back; and drop the node, because
	// whatever it is holding is either nothing or a texture that will never be
	// written to again. Without this the surface sits there publishing an
	// unwritten texture and the desktop is simply black, with `rendered` true and
	// nothing in the log to act on.
	if (this->mThread && this->mThread->failed() && !this->mFailSeen) {
		this->mFailSeen = true;
		delete node;
		node = nullptr;
		QMetaObject::invokeMethod(
		    this,
		    [this] {
			    if (!this->mFailed) {
				    this->mFailed = true;
				    emit this->failedChanged();
			    }
		    },
		    Qt::QueuedConnection
		);
		return nullptr;
	}

	GLuint texId = this->mThread ? this->mThread->acquireTexture() : 0;
	if (texId == 0) return node; // nothing ready yet; the first publish wakes us

	// First real frame of this project: tell QML (on the GUI thread) so a
	// wallpaper transition can start against actual content, not a black frame.
	if (!this->mLoadFrameSeen) {
		this->mLoadFrameSeen = true;
		QMetaObject::invokeMethod(
		    this,
		    [this] {
			    if (!this->mRendered) {
				    this->mRendered = true;
				    emit this->renderedChanged();
			    }
		    },
		    Qt::QueuedConnection
		);
	}

	if (!node) {
		node = new QSGSimpleTextureNode();
		node->setOwnsTexture(true);
		node->setFiltering(QSGTexture::Linear);
		// WE's scene FBO is already oriented for GL sampling; no extra vertical
		// mirror (the driver-output path needed one, the scene-FBO path does not).
	}

	// Wrap WE's GL texture (valid in the shared context) as a scene-graph texture.
	// TextureIsOpaque: a wallpaper is the bottom layer - ignore WE's alpha so the
	// desktop behind the window never shows through where a scene composites with
	// alpha < 1.
	auto* qtTex = QNativeInterface::QSGOpenGLTexture::fromNative(
	    texId,
	    this->window(),
	    QSize(this->mThread->width(), this->mThread->height()),
	    QQuickWindow::TextureIsOpaque
	);
	node->setTexture(qtTex); // ownsTexture => deletes the previous wrapper
	node->setRect(0, 0, w, h);
	return node;
}

void WallpaperEngineSurface::setProjectPath(const QString& projectPath) {
	if (projectPath == this->mProjectPath) return;
	this->mProjectPath = projectPath;
	emit this->projectPathChanged();
	if (this->mRendered) {
		this->mRendered = false;
		emit this->renderedChanged();
	}
	// A new project gets a clean slate: the previous one failing says nothing
	// about this one, and leaving `failed` set would strand the shell on the
	// static image for the rest of the session.
	if (this->mFailed) {
		this->mFailed = false;
		emit this->failedChanged();
	}
	this->updateFrameDriver();
	this->update();
}

void WallpaperEngineSurface::setLive(bool live) {
	if (live == this->mLive) return;
	this->mLive = live;
	emit this->liveChanged();
	this->updateFrameDriver();
	this->update();
}

void WallpaperEngineSurface::setFps(int fps) {
	if (fps == this->mFps) return;
	this->mFps = fps;
	emit this->fpsChanged();
	// Nothing to do on this side: fps is the producer's rate, and the surface
	// follows the producer.
	if (this->mThread) this->mThread->setFps(fps);
}

void WallpaperEngineSurface::setScaleMode(const QString& scaleMode) {
	if (scaleMode == this->mScaleMode) return;
	this->mScaleMode = scaleMode;
	emit this->scaleModeChanged();
	// Scaling is a WE startup argument; rebuild the thread so it takes effect.
	this->mLoadedPath.clear();
	this->update();
}

void WallpaperEngineSurface::setAudioEnabled(bool audioEnabled) {
	if (audioEnabled == this->mAudioEnabled) return;
	this->mAudioEnabled = audioEnabled;
	emit this->audioEnabledChanged();
	// Audio existence is a WE load-time decision (--silent skips stream/volume
	// setup entirely); rebuild the thread so the toggle takes effect.
	this->mLoadedPath.clear();
	this->update();
}

} // namespace qs::wallpaperengine
