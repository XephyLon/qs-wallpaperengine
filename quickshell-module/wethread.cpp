// glew before any Qt/GL header.
#include <GL/glew.h>

#include "wethread.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <csignal>
#include <pthread.h>

#include <chrono>
#include <clocale>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <glm/vec2.hpp>

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
	static thread_local bool registered = false;
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

// One color texture + FBO (+ depth/stencil) that WE composites into.
struct RenderTarget {
	GLuint texture = 0;
	GLuint fbo = 0;
	GLuint depthStencil = 0;

	void create(int w, int h) {
		glGenTextures(1, &this->texture);
		glBindTexture(GL_TEXTURE_2D, this->texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		glGenRenderbuffers(1, &this->depthStencil);
		glBindRenderbuffer(GL_RENDERBUFFER, this->depthStencil);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);

		glGenFramebuffers(1, &this->fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, this->fbo);
		glFramebufferTexture2D(
		    GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->texture, 0
		);
		glFramebufferRenderbuffer(
		    GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, this->depthStencil
		);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
};

} // namespace

WeThread::WeThread(
    void* display,
    void* sharedContext,
    std::string projectPath,
    std::string assetsDir,
    int width,
    int height,
    int fps,
    std::string scaleMode
)
    : mDisplay(display)
    , mContext(sharedContext)
    , mProjectPath(std::move(projectPath))
    , mAssetsDir(std::move(assetsDir))
    , mScaleMode(std::move(scaleMode))
    , mWidth(width)
    , mHeight(height)
    , mFps(fps > 0 ? fps : 60) {
	this->mThread = std::thread([this] { this->run(); });
}

WeThread::~WeThread() {
	this->mStop = true;
	if (this->mThread.joinable()) this->mThread.join();
}

unsigned int WeThread::acquireTexture() {
	if (!this->mReady) return 0;
	GLuint tex;
	GLsync sync;
	{
		std::lock_guard lock(this->mMutex);
		tex = this->mFrontTexture;
		sync = static_cast<GLsync>(this->mFrontFence);
	}
	// Server-side wait: Qt's context won't sample until WE's frame is complete.
	// Does not block the CPU.
	if (sync) glWaitSync(sync, 0, GL_TIMEOUT_IGNORED);
	return tex;
}

void WeThread::run() {
	// Block async-termination signals in this worker thread so the kernel
	// delivers them to Quickshell's main/handler thread instead. Otherwise this
	// thread (spawned with signals unblocked) can absorb SIGTERM/SIGINT and the
	// shell never quits (appears to ignore the signal).
	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGTERM);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGHUP);
	sigaddset(&sigs, SIGQUIT);
	pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

	// WE initializes SDL, which by default installs its own SIGINT/SIGTERM
	// handlers - they swallow those signals so the shell stops quitting on them.
	// Disable that before WE (SDL_Init) runs so the process terminates normally.
	setenv("SDL_NO_SIGNAL_HANDLERS", "1", 1);

	auto dpy = static_cast<EGLDisplay>(this->mDisplay);
	auto ctx = static_cast<EGLContext>(this->mContext);

	// mpv (WE's video-wallpaper backend) hard-requires LC_NUMERIC=C, and refuses
	// to create its context otherwise ("Non-C locale detected"). WE's own main()
	// sets this; we bypass main, so set it here. LC_NUMERIC only affects C-library
	// number formatting - Qt's UI uses QLocale independently, so the shell is
	// unaffected.
	std::setlocale(LC_NUMERIC, "C");

	// ctx is a Qt-built share context (matches Qt's config/robustness flags).
	// Make it current surfacelessly - we only render to FBOs, no window surface.
	eglBindAPI(EGL_OPENGL_API);
	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		std::fprintf(stderr, "WeThread: eglMakeCurrent failed: 0x%x\n", eglGetError());
		std::fflush(stderr);
		return;
	}

	glewExperimental = GL_TRUE;
	// GLEW_ERROR_NO_GLX_DISPLAY is expected under Wayland/EGL (no GLX) and is
	// non-fatal - core GL entry points still load. Only a hard failure aborts.
	if (GLenum err = glewInit(); err != GLEW_OK && err != GLEW_ERROR_NO_GLX_DISPLAY) {
		std::fprintf(stderr, "WeThread: glewInit failed: %s\n", glewGetErrorString(err));
		std::fflush(stderr);
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		return;
	}
	if (glGenFramebuffers == nullptr) {
		std::fprintf(stderr, "WeThread: core GL not loaded after glewInit\n");
		std::fflush(stderr);
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		return;
	}

	RenderTarget targets[2];
	targets[0].create(this->mWidth, this->mHeight);
	targets[1].create(this->mWidth, this->mHeight);
	int back = 0;

	// Build the WE app on THIS thread/context.
	ensureDriverRegistered();
	const std::string geo = "0x0x" + std::to_string(this->mWidth) + "x" + std::to_string(this->mHeight);
	std::vector<char*> argv {
	    const_cast<char*>("linux-wallpaperengine"),
	    const_cast<char*>("--window"),
	    const_cast<char*>(geo.c_str()),
	    const_cast<char*>("--silent"),
	    const_cast<char*>("--assets-dir"),
	    const_cast<char*>(this->mAssetsDir.c_str()),
	};
	if (!this->mScaleMode.empty()) {
		argv.push_back(const_cast<char*>("--scaling"));
		argv.push_back(const_cast<char*>(this->mScaleMode.c_str()));
	}
	argv.push_back(const_cast<char*>(this->mProjectPath.c_str()));

	std::unique_ptr<we::Application::ApplicationContext> appContext;
	std::unique_ptr<we::Application::WallpaperApplication> app;
	try {
		appContext = std::make_unique<we::Application::ApplicationContext>(
		    static_cast<int>(argv.size()), argv.data()
		);
		appContext->loadSettingsFromArgv();
		app = std::make_unique<we::Application::WallpaperApplication>(*appContext);
		app->setup();
	} catch (const std::exception& e) {
		std::fprintf(stderr, "WeThread: WE start failed: %s\n", e.what());
		std::fflush(stderr);
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		return;
	}
	auto* driver = pendingDriverSlot();
	pendingDriverSlot() = nullptr;
	if (driver == nullptr) {
		std::fprintf(stderr, "WeThread: driver never registered\n");
		std::fflush(stderr);
		app->cleanup();
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		return;
	}

	using clock = std::chrono::steady_clock;

	while (!this->mStop) {
		auto start = clock::now();
		const auto frameTime = std::chrono::milliseconds(1000 / this->mFps.load());

		auto& tgt = targets[back];
		// app->render() advances g_Time (driver clock - else the scene freezes at
		// t=0), updates audio/media, and drives the per-frame render + frame
		// counter. WE renders scenes into the wallpaper's OWN scene FBO
		// (getFirstWallpaperFramebuffer); the setDestinationFramebuffer composite
		// only works for some types (video), so blit the scene FBO into our
		// double-buffered target - reliable for both scene and video.
		app->render();
		GLuint srcFb = app->getFirstWallpaperFramebuffer();
		if (srcFb == 0) srcFb = driver->fbo();
		glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFb);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tgt.fbo);
		glBlitFramebuffer(
		    0, 0, this->mWidth, this->mHeight, 0, 0, this->mWidth, this->mHeight,
		    GL_COLOR_BUFFER_BIT, GL_LINEAR
		);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

		GLsync oldSync = nullptr;
		{
			std::lock_guard lock(this->mMutex);
			oldSync = static_cast<GLsync>(this->mFrontFence);
			this->mFrontTexture = tgt.texture;
			this->mFrontFence = sync;
		}
		if (oldSync) glDeleteSync(oldSync);
		this->mReady = true;
		back ^= 1;

		auto elapsed = clock::now() - start;
		if (elapsed < frameTime) std::this_thread::sleep_for(frameTime - elapsed);
	}

	app->cleanup();
	{
		std::lock_guard lock(this->mMutex);
		if (this->mFrontFence) glDeleteSync(static_cast<GLsync>(this->mFrontFence));
		this->mFrontFence = nullptr;
		this->mFrontTexture = 0;
	}
	glDeleteFramebuffers(1, &targets[0].fbo);
	glDeleteFramebuffers(1, &targets[1].fbo);
	glDeleteTextures(1, &targets[0].texture);
	glDeleteTextures(1, &targets[1].texture);
	glDeleteRenderbuffers(1, &targets[0].depthStencil);
	glDeleteRenderbuffers(1, &targets[1].depthStencil);
	app.reset();
	// ctx is owned by the surface's QOpenGLContext; just release it from this
	// thread, don't destroy it.
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

} // namespace qs::wallpaperengine
