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
#include <future>
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

	// Returns false if the target is unusable, in which case nothing may be
	// rendered into it.
	//
	// This check is the whole difference between "this wallpaper failed" and "the
	// desktop is black and nothing in the log says why". A full-screen RGBA8
	// colour attachment plus a D24S8 renderbuffer is ~59MB at 5120x1440, and this
	// allocation happens AFTER the scene it belongs to has taken its own share:
	// linux-wallpaperengine gives every image element two composite FBOs at
	// element size plus one per effect, so a scene whose source texture is 8192x4096
	// has already spent hundreds of megabytes before we ask for ours. GL does not
	// report that shortfall by failing glTexImage2D - it reports it by leaving the
	// framebuffer INCOMPLETE. Every later glBlitFramebuffer into an incomplete
	// framebuffer is a no-op that raises GL_INVALID_FRAMEBUFFER_OPERATION and
	// nothing else, so without this check the loop below happily publishes a
	// texture that is never written to, the surface happily samples it, and the
	// user gets black with a clean log.
	bool create(int w, int h) {
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

		const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			std::fprintf(
			    stderr, "WeThread: %dx%d render target incomplete (0x%x); wallpaper cannot render\n",
			    w, h, status
			);
			std::fflush(stderr);
			return false;
		}
		return true;
	}

	// Safe to call on a half-built or never-built target: the ids are 0 until
	// their glGen* succeeded, and 0 is never passed to glDelete*.
	void destroy() {
		if (this->fbo != 0) glDeleteFramebuffers(1, &this->fbo);
		if (this->depthStencil != 0) glDeleteRenderbuffers(1, &this->depthStencil);
		if (this->texture != 0) glDeleteTextures(1, &this->texture);
		this->fbo = 0;
		this->depthStencil = 0;
		this->texture = 0;
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
    std::string scaleMode,
    bool audioEnabled,
    std::function<void()> onFrame
)
    : mDisplay(display)
    , mContext(sharedContext)
    , mProjectPath(std::move(projectPath))
    , mAssetsDir(std::move(assetsDir))
    , mScaleMode(std::move(scaleMode))
    , mWidth(width)
    , mHeight(height)
    , mAudioEnabled(audioEnabled)
    , mOnFrame(std::move(onFrame))
    , mFps(fps > 0 ? fps : 60) {
	this->mThread = std::thread([this] { this->run(); });
}

WeThread::~WeThread() {
	this->mStop = true;
	if (!this->mThread.joinable()) return;

	// A plain join() here can deadlock the caller. mStop only breaks the render
	// loop AFTER app->setup() returns; a scene that hangs INSIDE setup() (a bad
	// pkg, an infinite loop in WE) never observes mStop, so join() would block
	// forever. This destructor runs on the GUI thread (aboutToQuit) or the render
	// thread (a project switch), so a wedged WE thread would freeze the whole
	// shell. Bound the join: hand the thread to a reaper and wait a few seconds;
	// if WE still has not stopped, detach and let the host carry on (the wedged
	// thread and its GL context leak, but the shell stays responsive). Normal
	// shutdowns finish well under the timeout - the render loop sees mStop on its
	// next frame - so this adds no latency to the common path.
	auto done = std::make_shared<std::promise<void>>();
	auto future = done->get_future();
	std::thread reaper([t = std::move(this->mThread), done]() mutable {
		t.join();
		done->set_value();
	});
	if (future.wait_for(std::chrono::seconds(3)) == std::future_status::ready) {
		reaper.join();
	} else {
		// Record the detach BEFORE returning. The owner reads this flag through
		// the shared_ptr it took a copy of, and it decides whether it is still
		// allowed to destroy the QOpenGLContext whose EGLContext this thread is
		// holding current. It is not: see WallpaperEngineSurface::releaseThread.
		std::fprintf(stderr, "WeThread: render thread did not stop in time; detaching\n");
		std::fflush(stderr);
		this->mDetached->store(true);
		reaper.detach();
	}
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

	// Declared up here so every failure path below can free whatever was built.
	RenderTarget targets[2];

	// Every early return past this point has to release the context AND free the
	// GL objects it already made. They used to just `return`, and that is the one
	// way a failing wallpaper genuinely reaches the next one: the render targets
	// live in the process-global SHARE group, so they outlive the context that
	// created them (the global share context lives until the shell quits). At
	// 5120x1440 a failed load stranded ~118MB of VRAM permanently, every time.
	// Flip through a handful of wallpapers that fail this way and the card is
	// full, at which point wallpapers that are perfectly fine start failing to
	// allocate too - "corruption spreads", with no corruption involved.
	auto failLoad = [&](const char* why) {
		std::fprintf(stderr, "WeThread: %s\n", why);
		std::fflush(stderr);
		this->mFailed.store(true);
		targets[0].destroy();
		targets[1].destroy();
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		// No frame will ever be published, so this is the only wake-up the
		// consumer gets: without it the failure latch is never read and the shell
		// never falls back to the static wallpaper.
		this->notify();
	};

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
		this->mFailed.store(true);
		this->notify();
		return;
	}

	glewExperimental = GL_TRUE;
	// GLEW_ERROR_NO_GLX_DISPLAY is expected under Wayland/EGL (no GLX) and is
	// non-fatal - core GL entry points still load. Only a hard failure aborts.
	if (GLenum err = glewInit(); err != GLEW_OK && err != GLEW_ERROR_NO_GLX_DISPLAY) {
		std::fprintf(stderr, "WeThread: glewInit failed: %s\n", glewGetErrorString(err));
		std::fflush(stderr);
		this->mFailed.store(true);
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		this->notify();
		return;
	}
	if (glGenFramebuffers == nullptr) {
		std::fprintf(stderr, "WeThread: core GL not loaded after glewInit\n");
		std::fflush(stderr);
		this->mFailed.store(true);
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		this->notify();
		return;
	}

	if (!targets[0].create(this->mWidth, this->mHeight)
	    || !targets[1].create(this->mWidth, this->mHeight))
	{
		failLoad("render targets unusable");
		return;
	}
	int back = 0;

	// Build the WE app on THIS thread/context.
	ensureDriverRegistered();
	const std::string geo = "0x0x" + std::to_string(this->mWidth) + "x" + std::to_string(this->mHeight);
	std::vector<char*> argv {
	    const_cast<char*>("linux-wallpaperengine"),
	    const_cast<char*>("--window"),
	    const_cast<char*>(geo.c_str()),
	    const_cast<char*>("--assets-dir"),
	    const_cast<char*>(this->mAssetsDir.c_str()),
	    // Pause only for a fullscreen window that is actually in front of the
	    // wallpaper. WE's detector defaults to counting EVERY fullscreen
	    // toplevel the compositor advertises - output, workspace and visibility
	    // are not part of the test - so a game parked on a workspace the user
	    // had left still froze the wallpaper they were looking at, on a still
	    // frame, until it stopped being fullscreen.
	    //
	    // Restricting the count to *activated* toplevels is the whole fix
	    // (isRelevant() drops a non-activated one): a fullscreen window holds
	    // activation only while it is focused, which is exactly when it covers
	    // the wallpaper. Switch away and it drops activation, so the wallpaper
	    // the user can now see resumes.
	    //
	    // This has to be WE's own pause rather than the shell's. `live` on the
	    // surface gates Qt's repaint timer and never reaches this thread, so it
	    // cannot idle a video wallpaper - only setPause() in here reaches mpv,
	    // and it is private to WallpaperApplication.
	    const_cast<char*>("--fullscreen-pause-only-active"),
	};
	// Audio is a load-time decision inside WE: with --silent, sound objects never
	// create their SDL streams and video wallpapers get mpv volume 0 at creation,
	// so it cannot be re-enabled at runtime - the surface rebuilds this thread to
	// flip it.
	//
	// When audio IS on: disable automute (WE mutes itself while ANY other
	// unmuted sink-input exists - virtual sinks/headset loopbacks trip it
	// permanently; the shell's own toggle is the mute control here) and run at
	// full internal volume (default is 15/128) so the per-stream system mixer
	// is the one volume knob. --volume and --silent are mutually exclusive to
	// WE's parser; only one branch ever adds its flags.
	if (this->mAudioEnabled) {
		argv.push_back(const_cast<char*>("--noautomute"));
		argv.push_back(const_cast<char*>("--volume"));
		argv.push_back(const_cast<char*>("128"));
	} else {
		argv.push_back(const_cast<char*>("--silent"));
	}
	// Pass the scaling mode: WE applies it for video wallpapers (which composite
	// into the driver output, not a scene FBO). Scenes render into their own
	// scene FBO at native projection and we scale those ourselves at blit time.
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
		// This is the path a scene too large for the GPU takes: WE validates its
		// OWN framebuffers (CFBO and the mpv player are the only two places it
		// calls glCheckFramebufferStatus) and turns a failure into a throw. It is
		// therefore the detector for "this wallpaper is too big to render", and
		// the reason `failed` is worth propagating: the shell can show the static
		// image instead of a black screen.
		//
		// app is deliberately not cleanup()'d here - it threw part-way through
		// setup(), so its invariants do not hold and cleanup() would be walking
		// half-built state. The unique_ptr destructor below is as far as we go.
		// WE strands some of its own GL objects on this path (its texture cache
		// and FBO registry have no eviction API); ours are freed by failLoad().
		std::fprintf(stderr, "WeThread: WE start failed: %s\n", e.what());
		std::fflush(stderr);
		failLoad("WE start failed");
		return;
	}
	auto* driver = pendingDriverSlot();
	pendingDriverSlot() = nullptr;
	if (driver == nullptr) {
		app->cleanup();
		failLoad("driver never registered");
		return;
	}

	// One-shot diagnostic, NOT a failure test. WE leaves the GL error queue dirty
	// as a matter of course and libmpv is the one that trips over it: mpv's
	// gl_check_error drains whatever WE left behind, concludes its own texture
	// allocation failed, and logs
	//   [libmpv_render] after creating texture: OpenGL error INVALID_FRAMEBUFFER_OPERATION
	//   Video: no video
	// on wallpapers that then play perfectly well. Draining here means that
	// message, when it does appear, is about something this frame did - so the
	// next person reading a bug report is not chasing a stale error from setup.
	if (GLenum err = glGetError(); err != GL_NO_ERROR) {
		std::fprintf(stderr, "WeThread: GL error queue dirty after setup: 0x%x (draining)\n", err);
		std::fflush(stderr);
		while (glGetError() != GL_NO_ERROR) {}
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
		const int srcW = app->getFirstWallpaperFramebufferWidth();
		const int srcH = app->getFirstWallpaperFramebufferHeight();
		// A real scene FBO (>= 64px) holds the wallpaper and we scale it ourselves
		// per mode. A tiny/absent one means a video wallpaper, which WE already
		// composited (scaled per --scaling) into the driver output FBO - blit that
		// straight across.
		// Fall back to the driver output only if there is genuinely no wallpaper
		// FBO yet (very first frames); otherwise scale the wallpaper's real-sized
		// scene/video FBO per the mode.
		const bool sceneValid = srcFb != 0 && srcW >= 8 && srcH >= 8;

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tgt.fbo);
		if (!sceneValid) {
			glBindFramebuffer(GL_READ_FRAMEBUFFER, driver->fbo());
			glBlitFramebuffer(
			    0, 0, this->mWidth, this->mHeight, 0, 0, this->mWidth, this->mHeight,
			    GL_COLOR_BUFFER_BIT, GL_LINEAR
			);
		} else {
			int sx0 = 0, sy0 = 0, sx1 = srcW, sy1 = srcH;
			int dx0 = 0, dy0 = 0, dx1 = this->mWidth, dy1 = this->mHeight;
			const double srcA = static_cast<double>(srcW) / srcH;
			const double dstA = static_cast<double>(this->mWidth) / this->mHeight;
			if (this->mScaleMode == "stretch") {
				// distort to fill: full source -> full target (defaults above).
			} else if (this->mScaleMode == "fit") {
				glClearColor(0.f, 0.f, 0.f, 1.f);
				glClear(GL_COLOR_BUFFER_BIT); // letterbox bars
				if (srcA > dstA) {
					const int h = static_cast<int>(this->mWidth / srcA);
					dy0 = (this->mHeight - h) / 2;
					dy1 = dy0 + h;
				} else {
					const int w = static_cast<int>(this->mHeight * srcA);
					dx0 = (this->mWidth - w) / 2;
					dx1 = dx0 + w;
				}
			} else { // "fill"/"default": cover, crop the source center
				if (srcA > dstA) {
					const int w = static_cast<int>(srcH * dstA);
					sx0 = (srcW - w) / 2;
					sx1 = sx0 + w;
				} else {
					const int h = static_cast<int>(srcW / dstA);
					sy0 = (srcH - h) / 2;
					sy1 = sy0 + h;
				}
			}
			glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFb);
			glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		// Flush so the fence is actually submitted to the GPU NOW. Without this
		// the fence sits in this context's unflushed command queue; the consumer
		// (Qt's render thread) glWaitSync()s on it cross-context and stalls until
		// this thread's next implicit flush. Scenes flush constantly so it went
		// unnoticed, but VIDEO wallpapers block on the CPU inside app->render()
		// for frame pacing (mpv), so the fence stayed unflushed for a whole video
		// period - throttling every shell window's render cycle (and with it the
		// QML animation driver) to the video's frame rate.
		glFlush();

		GLsync oldSync = nullptr;
		{
			std::lock_guard lock(this->mMutex);
			oldSync = static_cast<GLsync>(this->mFrontFence);
			this->mFrontTexture = tgt.texture;
			this->mFrontFence = sync;
		}
		if (oldSync) glDeleteSync(oldSync);
		this->mReady = true;
		// Published: wake the consumer. This, not a clock on the other side, is
		// what decides when the surface repaints and therefore when the
		// wallpaper's wl_surface commits.
		this->notify();
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
	targets[0].destroy();
	targets[1].destroy();
	app.reset();
	// ctx is owned by the surface's QOpenGLContext; just release it from this
	// thread, don't destroy it.
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

} // namespace qs::wallpaperengine
