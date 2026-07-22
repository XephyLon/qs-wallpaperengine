// glew before any Qt GL header.
#include <GL/glew.h>

#include "wallpaperenginesurface.hpp"

#include <qopenglframebufferobject.h>
#include <qquickframebufferobject.h>

#include <memory>
#include <string>
#include <vector>

#include <WallpaperEngine/Application/ApplicationContext.h>
#include <WallpaperEngine/Application/WallpaperApplication.h>
#include <WallpaperEngine/Render/Drivers/CFboOpenGLDriver.h>
#include <WallpaperEngine/Render/Drivers/VideoFactories.h>

namespace qs::wallpaperengine {

namespace {
namespace we = WallpaperEngine;

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
		    glm::ivec2 size {ctx.settings.render.window.geometry.z, ctx.settings.render.window.geometry.w};
		    if (size.x <= 0 || size.y <= 0) size = {1920, 1080};
		    auto driver =
		        std::make_unique<we::Render::Drivers::CFboOpenGLDriver>(ctx, app, nullptr, nullptr, size);
		    pendingDriverSlot() = driver.get();
		    return driver;
	    }
	);
}

// Builds + owns the WE app for one project. Lives on the render thread; all its
// GL runs inside QQuickFramebufferObject::Renderer::render() where Qt has made
// its context current, bound the target FBO, and bracketed the call.
class WallpaperEngineContext {
public:
	WallpaperEngineContext(const QString& projectPath, int width, int height) {
		ensureDriverRegistered();
		const std::string geo =
		    "0x0x" + std::to_string(width) + "x" + std::to_string(height);
		this->project = projectPath.toStdString();
		this->assets = std::string(qgetenv("HOME").constData())
		    + "/.local/share/Steam/steamapps/common/wallpaper_engine/assets";
		std::vector<char*> argv {
		    const_cast<char*>("linux-wallpaperengine"),
		    const_cast<char*>("--window"),
		    const_cast<char*>(geo.c_str()),
		    const_cast<char*>("--silent"),
		    const_cast<char*>("--assets-dir"),
		    const_cast<char*>(this->assets.c_str()),
		    const_cast<char*>(this->project.c_str()),
		};
		try {
			this->appContext = std::make_unique<we::Application::ApplicationContext>(
			    static_cast<int>(argv.size()), argv.data()
			);
			this->appContext->loadSettingsFromArgv();
			this->app = std::make_unique<we::Application::WallpaperApplication>(*this->appContext);
			this->app->setup();
			this->driver = pendingDriverSlot();
			pendingDriverSlot() = nullptr;
			this->ready = this->driver != nullptr;
		} catch (const std::exception& e) {
			qWarning("WallpaperEngineSurface: failed to start Wallpaper Engine: %s", e.what());
			this->ready = false;
		}
	}

	~WallpaperEngineContext() {
		if (this->app) this->app->cleanup();
	}

	// Render one frame into `targetFbo` (Qt's). Context is current + bracketed by
	// Qt (QQuickFramebufferObject).
	void renderInto(unsigned int targetFbo) {
		if (!this->ready) return;
		this->app->setDestinationFramebuffer(targetFbo);
		for (const auto& [screen, viewport] : this->app->getOutput().getViewports()) {
			this->app->update(viewport);
		}
	}

	[[nodiscard]] bool valid() const { return this->ready; }

private:
	std::string project;
	std::string assets;
	std::unique_ptr<we::Application::ApplicationContext> appContext;
	std::unique_ptr<we::Application::WallpaperApplication> app;
	we::Render::Drivers::CFboOpenGLDriver* driver = nullptr;
	bool ready = false;
};

// QQuickFramebufferObject renderer: Qt owns the FBO + context + state boundary.
class WeRenderer: public QQuickFramebufferObject::Renderer {
public:
	QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override {
		// WE needs a depth/stencil buffer for its scene rendering.
		QOpenGLFramebufferObjectFormat fmt;
		fmt.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
		return new QOpenGLFramebufferObject(size, fmt);
	}

	// synchronize() runs on the render thread with the GUI thread blocked and the
	// GL context current - Qt's designated place for GL resource setup. Build the
	// heavy WE app here (not in render()).
	void synchronize(QQuickFramebufferObject* item) override {
		auto* surface = static_cast<WallpaperEngineSurface*>(item);
		this->wantedPath = surface->projectPath();
		this->live = surface->live();
		const int w = static_cast<int>(surface->width());
		const int h = static_cast<int>(surface->height());
		if (this->wantedPath != this->loadedPath) this->context.reset();
		if (!this->context && !this->wantedPath.isEmpty() && w > 0 && h > 0) {
			this->loadedPath = this->wantedPath;
			this->context = std::make_unique<WallpaperEngineContext>(this->wantedPath, w, h);
			if (!this->context->valid()) this->context.reset();
		}
	}

	void render() override {
		auto* fbo = this->framebufferObject();
		if (this->context && fbo) this->context->renderInto(fbo->handle());
		if (this->live) this->update(); // schedule next frame
	}

private:
	std::unique_ptr<WallpaperEngineContext> context;
	QString wantedPath;
	QString loadedPath;
	bool live = true;
};
} // namespace

WallpaperEngineSurface::WallpaperEngineSurface(QQuickItem* parent): QQuickFramebufferObject(parent) {
	this->setMirrorVertically(true); // WE renders bottom-up
}

QQuickFramebufferObject::Renderer* WallpaperEngineSurface::createRenderer() const {
	return new WeRenderer();
}

void WallpaperEngineSurface::setProjectPath(const QString& projectPath) {
	if (projectPath == this->mProjectPath) return;
	this->mProjectPath = projectPath;
	emit this->projectPathChanged();
	this->update();
}

void WallpaperEngineSurface::setLive(bool live) {
	if (live == this->mLive) return;
	this->mLive = live;
	emit this->liveChanged();
	this->update();
}

void WallpaperEngineSurface::setFps(int fps) {
	if (fps == this->mFps) return;
	this->mFps = fps;
	emit this->fpsChanged();
}

} // namespace qs::wallpaperengine
