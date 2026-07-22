#include "wallpaperenginesurface.hpp"

#include <qquickitem.h>
#include <qquickwindow.h>
#include <qsgnode.h>
#include <qsgsimpletexturenode.h>
#include <qsgtexture.h>

// TODO(embed): include linux-wallpaperengine's public headers once the
// from-source build installs them, e.g.:
//   #include <WallpaperEngine/Application/WallpaperApplication.h>
//   #include <WallpaperEngine/Render/RenderContext.h>
//   #include "we-fbo-driver/CFboOutput.hpp"   // our VideoDriver/OutputViewport

namespace qs::wallpaperengine {

// WallpaperEngineContext owns everything WE-side: the WallpaperApplication, the
// RenderContext, and our CFboOutput VideoDriver that renders into an FBO on a GL
// context SHARED with Quickshell's QSGRenderContext. Kept in the .cpp so the
// header carries no WE types.
class WallpaperEngineContext {
public:
	explicit WallpaperEngineContext(QQuickWindow* window) : window(window) {
		// TODO(embed):
		// 1. Grab Quickshell's GL context/EGLDisplay from `window`
		//    (QSGRendererInterface::getResource(window, OpenGLContextResource)).
		// 2. Create a WE ApplicationContext in "window/offscreen" mode.
		// 3. Instantiate CFboOutput (our VideoDriver) bound to a shared GL
		//    context so the FBO's color texture is importable by Qt.
		// 4. Build RenderContext(videoDriver, application, mediaSource).
	}

	~WallpaperEngineContext() {
		// TODO(embed): tear down RenderContext, application, GL objects.
	}

	// Loads a project directory. Returns false on failure.
	bool load(const QString& /*projectPath*/) {
		// TODO(embed): application->setBackground(path); mark dirty.
		return false;
	}

	// Renders one frame into the FBO. Call on the render thread with the GL
	// context current. Sets `size` to the rendered dimensions.
	void renderFrame(QSize& /*size*/) {
		// TODO(embed): renderContext->render(&outputViewport);
		// The CFboOutput viewport targets our FBO; after render, its color
		// attachment texture id is `textureId()`.
	}

	// GL texture id of the FBO color attachment (valid after renderFrame).
	[[nodiscard]] unsigned int textureId() const {
		return 0; // TODO(embed): return fbo color attachment
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
