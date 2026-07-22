#include "wallpaperenginesurface.hpp"

#include <qquickitem.h>
#include <qquickwindow.h>
#include <qsgnode.h>
#include <qsgsimpletexturenode.h>
#include <qsgtexture.h>

// TODO(embed): include linux-wallpaperengine's headers from the from-source tree:
//   #include <WallpaperEngine/Application/ApplicationContext.h>
//   #include <WallpaperEngine/Application/WallpaperApplication.h>
//   #include <WallpaperEngine/Render/RenderContext.h>
//   #include <WallpaperEngine/Render/Drivers/CFboOpenGLDriver.h>  // our driver

namespace qs::wallpaperengine {

// WallpaperEngineContext owns everything WE-side: the WallpaperApplication, the
// RenderContext, and our CFboOutput VideoDriver that renders into an FBO on a GL
// context SHARED with Quickshell's QSGRenderContext. Kept in the .cpp so the
// header carries no WE types.
class WallpaperEngineContext {
public:
	explicit WallpaperEngineContext(QQuickWindow* window) : window(window) {
		// Simplified path (thanks to WallpaperApplication::setDestinationFramebuffer):
		// run WE on Qt's OWN GL context - no separate/shared EGL context needed.
		// TODO(embed), all on the render thread with Qt's context current
		// (bracket WE's GL with window->beginExternalCommands()/endExternalCommands()):
		// 1. Build a WE ApplicationContext for this project (path, fps, scaling)
		//    + WallpaperApplication. CFboOpenGLDriver adopts the current (Qt)
		//    context: makeCurrent no-op, getProcAddress via eglGetProcAddress.
		// 2. Create a GL texture + FBO (mColorTexture/mFbo) sized to the wallpaper.
		// 3. app->setDestinationFramebuffer(mFbo) -> WE composites its final frame
		//    into our texture (CWallpaper binds m_destFramebuffer before compositing).
	}

	~WallpaperEngineContext() {
		// TODO(embed): tear down RenderContext, application, GL objects.
	}

	// Loads a project directory. Returns false on failure.
	bool load(const QString& /*projectPath*/) {
		// TODO(embed): application->setBackground(path); mark dirty.
		return false;
	}

	// Renders one frame into the destination FBO. Call on the render thread with
	// Qt's context current. Sets `size` to the rendered dimensions.
	void renderFrame(QSize& /*size*/) {
		// TODO(embed): for each viewport in app->getOutput().getViewports():
		//   app->update(viewport);   // == m_renderContext->render(viewport)
		// WE composites into mFbo (set via setDestinationFramebuffer). Wrap in
		// window->beginExternalCommands()/endExternalCommands() so Qt restores
		// its own GL state afterwards. size = app->getOutput() full size.
	}

	// GL texture id of the destination FBO color attachment (valid after a frame).
	[[nodiscard]] unsigned int textureId() const {
		return 0; // TODO(embed): return mColorTexture
	}

	[[nodiscard]] bool valid() const { return this->ready; }

private:
	QQuickWindow* window;
	bool ready = false;
	// TODO(embed): CFboOutput* output; RenderContext* renderContext;
	//              WallpaperApplication* application; unsigned int fbo, tex;
};

WallpaperEngineSurface::WallpaperEngineSurface(QQuickItem* parent): QQuickItem(parent) {
	this->setFlag(QQuickItem::ItemHasContents, true);
}

WallpaperEngineSurface::~WallpaperEngineSurface() { this->destroyContext(); }

void WallpaperEngineSurface::componentComplete() {
	this->QQuickItem::componentComplete();
	this->completed = true;
	// Defer context creation to first render: the QQuickWindow (and its GL
	// context) is guaranteed available on the render thread by then.
	if (!this->mProjectPath.isEmpty()) this->update();
}

void WallpaperEngineSurface::setProjectPath(const QString& projectPath) {
	if (projectPath == this->mProjectPath) return;
	this->mProjectPath = projectPath;
	emit this->projectPathChanged();
	if (this->completed) this->reload();
}

void WallpaperEngineSurface::setLive(bool live) {
	if (live == this->mLive) return;
	this->mLive = live;
	emit this->liveChanged();
	if (live) this->update();
}

void WallpaperEngineSurface::setFps(int fps) {
	if (fps == this->mFps) return;
	this->mFps = fps;
	emit this->fpsChanged();
}

void WallpaperEngineSurface::createContext() {
	if (this->context || !this->window()) return;
	this->context = new WallpaperEngineContext(this->window());
	// Drive frames from the render thread's beforeRendering hook so the FBO is
	// fresh each Qt frame without a separate GL context switch.
	QObject::connect(
	    this->window(),
	    &QQuickWindow::beforeRendering,
	    this,
	    &WallpaperEngineSurface::onBeforeRendering,
	    Qt::DirectConnection
	);
	this->reload();
}

void WallpaperEngineSurface::destroyContext() {
	if (this->window()) this->window()->disconnect(this);
	delete this->context;
	this->context = nullptr;
	this->bHasContent = false;
}

void WallpaperEngineSurface::reload() {
	if (!this->context) return;
	auto ok = this->context->load(this->mProjectPath);
	if (!ok) {
		this->bHasContent = false;
		emit this->stopped();
	}
	this->update();
}

void WallpaperEngineSurface::onBeforeRendering() {
	if (!this->context || !this->context->valid()) return;
	if (!this->mLive && this->bHasContent) return; // single-frame mode: render once

	QSize size;
	this->context->renderFrame(size);
	if (size.isValid() && size != this->bSourceSize.value()) this->bSourceSize = size;
	if (!this->bHasContent.value()) this->bHasContent = true;

	if (this->mLive) this->update(); // schedule the next frame
}

QSGNode* WallpaperEngineSurface::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* /*unused*/) {
	// Lazily build the WE context on the render thread (GL context is current).
	if (!this->context) this->createContext();

	if (!this->context || !this->bHasContent.value()) {
		delete oldNode;
		return nullptr;
	}

	auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
	if (!node) node = new QSGSimpleTextureNode();

	// Wrap the WE FBO's GL texture as a QSGTexture without copying.
	auto* texture = this->window()->createTextureFromNativeObject(
	    QQuickWindow::NativeObjectTexture,
	    // NOLINTNEXTLINE: WE hands us a raw GLuint
	    reinterpret_cast<const void*>(static_cast<quintptr>(this->context->textureId())),
	    0,
	    this->bSourceSize.value()
	);

	node->setOwnsTexture(true);
	node->setTexture(texture);
	node->setRect(this->boundingRect());
	node->setFiltering(QSGTexture::Linear);
	return node;
}

} // namespace qs::wallpaperengine
