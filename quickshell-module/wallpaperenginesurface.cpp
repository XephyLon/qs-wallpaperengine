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
	// The WE thread produces frames on its own; the surface just repaints at the
	// display cadence. Driving update() from a GUI-thread timer (not from inside
	// updatePaintNode on the render thread) keeps the scene-graph sync race-free.
	QObject::connect(&this->mRepaint, &QTimer::timeout, this, [this] { this->update(); });
	this->updateRepaintTimer();

	// Stop + join the WE thread while the event loop and render thread are still
	// alive. If we wait for the item destructor (Qt teardown), the scene-graph
	// render thread deadlocks against the still-running WE thread and the process
	// hangs on quit (SIGTERM appears ignored). aboutToQuit fires first, on the
	// GUI thread, so the join happens at a safe point.
	QObject::connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
		this->mThread.reset();
		this->mShareContext.reset();
	});
}

void WallpaperEngineSurface::updateRepaintTimer() {
	if (this->mLive && !this->mProjectPath.isEmpty()) {
		this->mRepaint.setInterval(1000 / (this->mFps > 0 ? this->mFps : 60));
		if (!this->mRepaint.isActive()) this->mRepaint.start();
	} else {
		this->mRepaint.stop();
	}
}

WallpaperEngineSurface::~WallpaperEngineSurface() = default;

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
	// Rebuild not only on a project switch but whenever Qt's current GL context
	// differs from the one we built against: Hyprland's fullscreen direct-scanout
	// can make Qt destroy and recreate this window's scene-graph context, which
	// orphans our shared EGLContext and leaves WE's texture invalid in the new
	// context (the wallpaper "breaks" and stays broken). Rebuilding here - on the
	// render thread, with the new context current - re-shares against it and
	// recovers automatically.
	auto* qtCtx = QOpenGLContext::currentContext();
	// Pointer inequality alone is not enough: a destroy+recreate can hand the
	// new QOpenGLContext the old one's heap address, so mContextLost (latched by
	// aboutToBeDestroyed on the adopted context) is the authoritative signal.
	const bool contextChanged = this->mThread && (this->mLoadedContext != qtCtx || this->mContextLost);
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
		this->mThread.reset();
		this->mShareContext.reset();

		auto share = std::make_unique<QOpenGLContext>();
		share->setFormat(qtCtx->format());
		share->setShareContext(qtCtx);
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

		// Adopting a (possibly new) context: latch its destruction so the next
		// updatePaintNode rebuilds even if the replacement reuses this address.
		// aboutToBeDestroyed fires on the context's thread (the render thread,
		// same as this code), so a plain bool is safe. Reconnecting on a
		// same-context project switch would stack duplicate connections, so
		// only connect when the context is actually new.
		if (this->mLoadedContext != qtCtx || this->mContextLost) {
			QObject::connect(qtCtx, &QOpenGLContext::aboutToBeDestroyed, this, [this] {
				this->mContextLost = true;
			}, Qt::DirectConnection);
		}
		this->mContextLost = false;
		this->mShareContext = std::move(share);
		this->mLoadedPath = this->mProjectPath;
		this->mLoadedContext = qtCtx;
		this->mLoadFrameSeen = false; // new project: re-arm the first-frame latch
		this->mThread = std::make_unique<WeThread>(
		    dpy, eglCtx->nativeContext(), this->mProjectPath.toStdString(), assetsDir(), w, h,
		    this->mFps, this->mScaleMode.toStdString(), this->mAudioEnabled
		);
	}

	GLuint texId = this->mThread ? this->mThread->acquireTexture() : 0;
	if (texId == 0) return node; // nothing ready yet; timer will retry

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
	this->updateRepaintTimer();
	this->update();
}

void WallpaperEngineSurface::setLive(bool live) {
	if (live == this->mLive) return;
	this->mLive = live;
	emit this->liveChanged();
	this->updateRepaintTimer();
	this->update();
}

void WallpaperEngineSurface::setFps(int fps) {
	if (fps == this->mFps) return;
	this->mFps = fps;
	emit this->fpsChanged();
	this->updateRepaintTimer();
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
