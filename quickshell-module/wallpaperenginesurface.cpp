#include "wallpaperenginesurface.hpp"

#include <qquickitem.h>
#include <qquickwindow.h>
#include <qsgnode.h>
#include <qsgsimpletexturenode.h>
#include <qsgtexture.h>

#include <memory>
#include <string>
#include <vector>

// linux-wallpaperengine (from-source tree; the packaged -git omits these headers)
#include <WallpaperEngine/Application/ApplicationContext.h>
#include <WallpaperEngine/Application/WallpaperApplication.h>
#include <WallpaperEngine/Render/Drivers/CFboOpenGLDriver.h>
#include <WallpaperEngine/Render/Drivers/VideoFactories.h>

namespace qs::wallpaperengine {

namespace {
namespace we = WallpaperEngine;

// Register our FBO driver for EXPLICIT_WINDOW once. Runs after WE's own
// __attribute__((constructor)) registrations, so it overrides GLFW for that
// mode. The factory stashes the created driver into a thread-local slot so the
// context that triggered setup() can reach it (fbo()/texture()).
we::Render::Drivers::CFboOpenGLDriver*& pendingDriverSlot() {
	static thread_local we::Render::Drivers::CFboOpenGLDriver* slot = nullptr;
	return slot;
}

void ensureDriverRegistered() {
	static bool registered = false;
	if (registered) return;
	registered = true;
	sVideoFactories.registerDriver(
	    we::Application::ApplicationContext::EXPLICIT_WINDOW,
	    DEFAULT_WINDOW_NAME,
	    [](we::Application::ApplicationContext& ctx, we::Application::WallpaperApplication& app)
	        -> std::unique_ptr<we::Render::Drivers::VideoDriver> {
		    // Size comes from --window geometry parsed into the context; adopt
		    // the current (Qt) context, so no share display/context.
		    // TODO(embed): read the parsed window size from `ctx` instead of a
		    // placeholder; ApplicationContext stores the EXPLICIT_WINDOW size.
		    glm::ivec2 size {1920, 1080};
		    auto driver = std::make_unique<we::Render::Drivers::CFboOpenGLDriver>(
		        ctx, app, nullptr, nullptr, size
		    );
		    pendingDriverSlot() = driver.get();
		    return driver;
	    }
	);
}
} // namespace

// Owns the WE app + our driver. Everything here runs on the scene-graph render
// thread with Qt's GL context current (bracket WE's GL with the window's
// beginExternalCommands()/endExternalCommands()).
class WallpaperEngineContext {
public:
	WallpaperEngineContext(QQuickWindow* window, const QString& projectPath, int width, int height)
	    : window(window) {
		ensureDriverRegistered();

		// Synthesize argv so WE's normal CLI parser fills ApplicationContext
		// (mode=EXPLICIT_WINDOW + geometry + project). Simplest correct path.
		const std::string geo =
		    "0x0x" + std::to_string(width) + "x" + std::to_string(height);
		this->project = projectPath.toStdString();
		std::vector<char*> argv {
		    const_cast<char*>("linux-wallpaperengine"),
		    const_cast<char*>("--window"),
		    const_cast<char*>(geo.c_str()),
		    const_cast<char*>("--silent"),
		    const_cast<char*>(this->project.c_str()),
		};

		this->window->beginExternalCommands();
		this->appContext = std::make_unique<we::Application::ApplicationContext>(
		    static_cast<int>(argv.size()), argv.data()
		);
		this->app = std::make_unique<we::Application::WallpaperApplication>(*this->appContext);
		this->app->setup(); // creates our driver (factory) + loads the wallpaper
		this->driver = pendingDriverSlot();
		pendingDriverSlot() = nullptr;
		if (this->driver) this->app->setDestinationFramebuffer(this->driver->fbo());
		this->window->endExternalCommands();

		this->ready = this->driver != nullptr;
	}

	~WallpaperEngineContext() {
		if (!this->app) return;
		this->window->beginExternalCommands();
		this->app->cleanup();
		this->app.reset();
		this->appContext.reset();
		this->window->endExternalCommands();
	}

	void renderFrame() {
		if (!this->ready) return;
		this->window->beginExternalCommands();
		// dispatchEventQueue() clears + runs app.update(viewport) into our FBO,
		// without swap/poll/sleep. WE composites into the destination FBO.
		this->driver->dispatchEventQueue();
		this->window->endExternalCommands();
	}

	[[nodiscard]] unsigned int textureId() const {
		return this->driver ? this->driver->texture() : 0;
	}

	[[nodiscard]] QSize size() const {
		if (!this->driver) return {};
		auto s = this->driver->getFramebufferSize();
		return {s.x, s.y};
	}

	[[nodiscard]] bool valid() const { return this->ready; }

private:
	QQuickWindow* window;
	std::string project;
	std::unique_ptr<we::Application::ApplicationContext> appContext;
	std::unique_ptr<we::Application::WallpaperApplication> app;
	we::Render::Drivers::CFboOpenGLDriver* driver = nullptr;
	bool ready = false;
};

WallpaperEngineSurface::WallpaperEngineSurface(QQuickItem* parent): QQuickItem(parent) {
	this->setFlag(QQuickItem::ItemHasContents, true);
}

WallpaperEngineSurface::~WallpaperEngineSurface() { this->destroyContext(); }

void WallpaperEngineSurface::componentComplete() {
	this->QQuickItem::componentComplete();
	this->completed = true;
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
	if (this->context || !this->window() || this->mProjectPath.isEmpty()) return;
	const auto w = static_cast<int>(this->width() > 0 ? this->width() : 1920);
	const auto h = static_cast<int>(this->height() > 0 ? this->height() : 1080);
	this->context = new WallpaperEngineContext(this->window(), this->mProjectPath, w, h);
	if (!this->context->valid()) {
		this->destroyContext();
		emit this->stopped();
		return;
	}
	QObject::connect(
	    this->window(),
	    &QQuickWindow::beforeRendering,
	    this,
	    &WallpaperEngineSurface::onBeforeRendering,
	    Qt::DirectConnection
	);
}

void WallpaperEngineSurface::destroyContext() {
	if (this->window()) this->window()->disconnect(this);
	delete this->context;
	this->context = nullptr;
	this->bHasContent = false;
}

void WallpaperEngineSurface::reload() {
	// A project change rebuilds the context (WE ties the loaded wallpaper to the
	// app instance). Cheap enough for the selector's occasional switches.
	this->destroyContext();
	this->update();
}

void WallpaperEngineSurface::onBeforeRendering() {
	if (!this->context || !this->context->valid()) return;
	if (!this->mLive && this->bHasContent.value()) return;

	this->context->renderFrame();
	const auto size = this->context->size();
	if (size.isValid() && size != this->bSourceSize.value()) this->bSourceSize = size;
	if (!this->bHasContent.value()) this->bHasContent = true;

	if (this->mLive) this->update();
}

QSGNode* WallpaperEngineSurface::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* /*unused*/) {
	if (!this->context) this->createContext();

	if (!this->context || !this->bHasContent.value()) {
		delete oldNode;
		return nullptr;
	}

	auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
	if (!node) node = new QSGSimpleTextureNode();

	auto* texture = QNativeInterface::QSGOpenGLTexture::fromNative(
	    this->context->textureId(),
	    this->window(),
	    this->bSourceSize.value()
	);

	node->setOwnsTexture(true);
	node->setTexture(texture);
	node->setRect(this->boundingRect());
	node->setFiltering(QSGTexture::Linear);
	// WE renders bottom-up (renderVFlip): flip the texture vertically.
	node->setTextureCoordinatesTransform(QSGSimpleTextureNode::MirrorVertically);
	return node;
}

} // namespace qs::wallpaperengine
