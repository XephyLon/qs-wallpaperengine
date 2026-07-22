// glew before any Qt/GL header.
#include <GL/glew.h>

#include "wallpaperenginesurface.hpp"
#include "wethread.hpp"

#include <EGL/egl.h>

#include <string>

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
	if (!this->mThread || this->mLoadedPath != this->mProjectPath) {
		this->mThread.reset();
		this->mShareContext.reset();

		auto* qtCtx = QOpenGLContext::currentContext();
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

		this->mShareContext = std::move(share);
		this->mLoadedPath = this->mProjectPath;
		this->mThread = std::make_unique<WeThread>(
		    dpy, eglCtx->nativeContext(), this->mProjectPath.toStdString(), assetsDir(), w, h
		);
	}

	GLuint texId = this->mThread ? this->mThread->acquireTexture() : 0;
	if (texId == 0) return node; // nothing ready yet; timer will retry

	if (!node) {
		node = new QSGSimpleTextureNode();
		node->setOwnsTexture(true);
		node->setFiltering(QSGTexture::Linear);
		node->setTextureCoordinatesTransform(QSGSimpleTextureNode::MirrorVertically);
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
}

} // namespace qs::wallpaperengine
