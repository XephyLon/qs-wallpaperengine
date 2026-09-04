// glew before any Qt/GL header.
#include <GL/glew.h>

#include "wethread.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <csignal>
#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include <glm/vec2.hpp>

#include <qimage.h>
#include <qstring.h>

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

// Producer frame rate while the shell reports this output occluded. Deliberately
// not zero - the occluded branch in run() has the argument for why the loop must
// keep calling app->render() at all. Four hertz is fifteen times cheaper than the
// default sixty and still drains mpv's render context often enough that it is
// never sitting on a stale update when the output comes back. The number itself
// is a guess: nothing here could profile what libmpv is comfortable with, so it
// is a named constant precisely so it can be raised if a video wallpaper is ever
// seen to hitch on resume.
constexpr int OCCLUDED_FPS = 4;

// Longest the pacing sleep may go without re-reading the stop and occlusion
// flags. One 60Hz frame: this sleep is the only thing standing between the shell
// clearing `occluded` and the first frame the user sees as a game closes, and
// that must not be visible against the compositor's own fullscreen-exit
// transition. It also bounds how long a shutdown sits inside an idle iteration,
// which at OCCLUDED_FPS would otherwise be a quarter of a second - and that comes
// straight off the bounded join's deadline in WeThread::stop().
constexpr auto PACE_SLICE = std::chrono::milliseconds(16);

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
    int volume,
    bool audioProcessing,
    bool mouseDisabled,
    bool parallaxDisabled,
    bool particlesDisabled,
    std::vector<std::string> setProperties,
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
    , mVolumeArg(std::to_string(std::clamp(volume, 0, 128)))
    , mAudioProcessing(audioProcessing)
    , mMouseDisabled(mouseDisabled)
    , mParallaxDisabled(parallaxDisabled)
    , mParticlesDisabled(particlesDisabled)
    , mSetProperties(std::move(setProperties))
    , mOnFrame(std::move(onFrame))
    , mFps(fps > 0 ? fps : 60) {
	this->mThread = std::thread([this] { this->run(); });
}

bool WeThread::stop() {
	this->mStop = true;
	// Already stopped once: mThread was moved into the reaper below, so this is
	// a repeat call and the first attempt's verdict still stands. A detached
	// thread never becomes joinable again, so waiting a second time cannot
	// change the answer.
	if (!this->mThread.joinable()) return !this->mDetached->load();

	// A plain join() here can deadlock the caller. mStop only breaks the render
	// loop AFTER app->setup() returns; a scene that hangs INSIDE setup() (a bad
	// pkg, an infinite loop in WE) never observes mStop, so join() would block
	// forever. This runs on the GUI thread (aboutToQuit) or the render thread (a
	// project switch), so a wedged WE thread would freeze the whole shell. Bound
	// the join: hand the thread to a reaper and wait a few seconds; if WE still
	// has not stopped, detach and let the host carry on (the wedged thread, its
	// GL context and this object leak, but the shell stays responsive). Normal
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
		return true;
	}

	// Detached - and that is a fact about THIS OBJECT, not just about the GL
	// context. run() is still executing, and everything it touches is a member
	// of this object: mStop, mFps, mScaleMode, mMutex, mFrontTexture,
	// mFrontFence, mOnFrame. So the owner may neither destroy nor free it, and
	// may not destroy the QOpenGLContext whose EGLContext the thread still has
	// current. Saying so is this return value's whole job: see
	// WallpaperEngineSurface::releaseThread.
	std::fprintf(stderr, "WeThread: render thread did not stop in time; detaching\n");
	std::fflush(stderr);
	this->mDetached->store(true);
	reaper.detach();
	return false;
}

// Correct only once stop() has returned true. It calls stop() itself so the
// ordinary path (already joined) needs no ceremony at the call site; a false
// here means the object is being freed out from under a live run(), and there
// is nothing this side can do about that except say so loudly.
WeThread::~WeThread() {
	if (!this->stop()) {
		std::fprintf(
		    stderr,
		    "WeThread: destroyed while its render thread is still running; the owner "
		    "must leak it instead of destroying it (see WeThread::stop)\n"
		);
		std::fflush(stderr);
	}
}

unsigned int WeThread::acquireTexture() {
	// Deliberately outside the lock. It is the path every frame of a failed load
	// and the first frames of every load take, and it never needs to see a
	// consistent texture/fence pair - only "has anything ever been published".
	if (!this->mReady) return 0;

	// The glWaitSync is issued INSIDE the lock, and that is load-bearing rather
	// than tidiness. Copying the fence handle out and waiting on it after the
	// unlock left a window the producer fits straight through: it takes the lock,
	// reads the same fence as the one it is about to replace, releases, and calls
	// glDeleteSync on it - all before this thread reaches the wait. The wait then
	// names an object that no longer exists, which is GL_INVALID_VALUE and NO
	// WAIT AT ALL (or, once the driver recycles the name for the next frame's
	// fence, a wait on the wrong frame). Since this wait is the only thing
	// stopping Qt from sampling a texture the producer is still blitting into,
	// what that buys is a torn wallpaper frame - top half new, bottom half
	// previous - with nothing anywhere to explain it: run() drains the GL error
	// queue once after setup and deliberately never looks again, so the tear is
	// the only evidence a hit leaves.
	//
	// Under the lock the two are totally ordered, and both orders are safe.
	// Either the wait goes in first, and the producer's later glDeleteSync is
	// deferred because a wait is already outstanding on the object; or the
	// producer publishes first, and this thread reads the NEW fence and never
	// names the deleted one. Holding the mutex across it costs nothing:
	// glWaitSync is a server-side wait that returns to the caller immediately -
	// Qt's context will not sample until WE's frame is complete, but no CPU
	// blocks here - and the producer's own critical section is three stores long.
	// This also closes the same race against the teardown block at the bottom of
	// run(), which deletes the front fence under this lock.
	std::lock_guard lock(this->mMutex);
	auto sync = static_cast<GLsync>(this->mFrontFence);
	if (sync) glWaitSync(sync, 0, GL_TIMEOUT_IGNORED);
	return this->mFrontTexture;
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
	    // Turn WE's fullscreen pause OFF and let the shell own the policy through
	    // `occluded`. This flag does not merely pick a predicate - it decides
	    // whether a detector is CONSTRUCTED AT ALL, and both halves of that
	    // matter here.
	    //
	    // Which output. WE's detector has no concept of one: its
	    // wlr_foreign_toplevel output-enter/output-leave handlers are empty
	    // stubs, so a toplevel's output is never recorded, and
	    // anythingFullscreen() is one flat process-wide count. The shell runs one
	    // of these threads per output, so every detector counts the same global
	    // set - a fullscreen window on ONE output froze the wallpaper on ALL of
	    // them, including the ones the user was looking at with nothing covering
	    // them. --fullscreen-pause-ignore-appid does not help; it matches on app
	    // id, not output.
	    //
	    // What it costs to ask - SECONDARY, and deliberately not the argument.
	    // With the pause enabled, createFullscreenDetector() builds a real
	    // WaylandFullScreenDetector, which opens its OWN wl_display_connect() - a
	    // second Wayland client connection per wallpaper thread, on top of Qt's -
	    // and does a wl_display_roundtrip() on every anythingFullscreen();
	    // WallpaperApplication::render() asks once per frame and this loop calls
	    // render() once per iteration. With the pause off the factory returns the
	    // no-op base FullScreenDetector, whose anythingFullscreen() is a `return
	    // false` with no I/O, so this thread does no Wayland work whatsoever - and
	    // --no-fullscreen-pause is the only spelling that clears the setting.
	    //
	    // That round-trip has NOT been measured to cost anything here. Wayland
	    // message traffic is cheap in CPU terms, and no attempt to isolate a
	    // per-frame cost from it has succeeded. It is written down because it
	    // explains what the flag actually switches, not because it justifies the
	    // change - the output-blindness above is the whole justification, and it
	    // would stand if the round-trip were free. Anyone tempted to re-enable the
	    // pause should argue with that paragraph, not this one.
	    //
	    // The compensating benefit - idling a wallpaper nobody can see - is not
	    // lost, it moves to the side that can actually tell: see setOccluded() and
	    // the occluded branch in the render loop below.
	    const_cast<char*>("--no-fullscreen-pause"),
	};
	// Audio is a load-time decision inside WE: with --silent, sound objects never
	// create their SDL streams and video wallpapers get mpv volume 0 at creation,
	// so it cannot be re-enabled at runtime - the surface rebuilds this thread to
	// flip it.
	//
	// Automute is ALWAYS disabled. It is not useful in an embedded surface: the
	// shell owns the audio toggle, and WE's detector mutes itself whenever any
	// other unmuted sink input exists. More importantly, leaving automute enabled
	// in the --silent branch still constructs PulseAudioPlayingDetector and makes
	// it enumerate the server and every sink input synchronously on every rendered
	// frame. A 150-second heaptrack run while investigating #16 recorded 10.7
	// million pa_xmalloc calls from that path. Removing the churn does not by
	// itself resolve that issue's remaining video-path growth. --silent
	// suppresses output, not the detector.
	argv.push_back(const_cast<char*>("--noautomute"));

	// When audio IS on, run at the volume the surface asked for (WE's own
	// 0..128 scale; the QML property is 0..100 and the surface maps it). The
	// old fixed 128 - "full internal volume so the per-stream system mixer is
	// the one knob" - survives as the DEFAULT the surface maps 100 to; a
	// per-wallpaper volume exists because one wallpaper's soundtrack being
	// mastered louder than another's is a property of the wallpaper, which the
	// per-stream mixer (one knob per app, and every wallpaper is the same app)
	// cannot express. --volume and --silent are mutually exclusive to WE's
	// parser; only one branch adds either flag.
	if (this->mAudioEnabled) {
		argv.push_back(const_cast<char*>("--volume"));
		argv.push_back(const_cast<char*>(this->mVolumeArg.c_str()));
	} else {
		argv.push_back(const_cast<char*>("--silent"));
	}
	// Audio-reactive effects sample the DESKTOP's audio through a recorder -
	// a wallpaper visualizing what is playing - which is independent of
	// whether the wallpaper's own soundtrack plays, so this sits outside the
	// volume/silent branch: a muted wallpaper can still react, and switching
	// the recorder off is a saving whether or not playback is on.
	if (!this->mAudioProcessing) {
		argv.push_back(const_cast<char*>("--no-audio-processing"));
	}
	// The three engine feature switches, load-time like everything else here.
	if (this->mMouseDisabled) argv.push_back(const_cast<char*>("--disable-mouse"));
	if (this->mParallaxDisabled) argv.push_back(const_cast<char*>("--disable-parallax"));
	if (this->mParticlesDisabled) argv.push_back(const_cast<char*>("--disable-particles"));
	// Per-wallpaper user properties from project.json's general.properties,
	// already serialized to WE's own "name=value" form. The strings live in
	// mSetProperties for the thread's whole life, so the pointers argv hands
	// WE stay valid exactly the way mScaleMode's does.
	for (const auto& property: this->mSetProperties) {
		argv.push_back(const_cast<char*>("--set-property"));
		argv.push_back(const_cast<char*>(property.c_str()));
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

	// Pace one iteration: sleep out the rest of its frame budget, but in slices,
	// so the loop can notice something that happened during the sleep. Two things
	// can. The shell uncovering this output, where the surface is still showing
	// the frame it was covered on, so every millisecond until the next publish is
	// a visible freeze as the game closes; and shutdown, which at OCCLUDED_FPS
	// would otherwise sit in here for a quarter of a second per iteration and eat
	// into the bounded join's deadline in stop().
	//
	// Slicing does not loosen the pacing: the deadline is recomputed from `start`
	// every time round, so a slice that oversleeps shortens the next one and only
	// the last one's overshoot leaks - exactly as with the single sleep this
	// replaces.
	auto pace = [this](clock::time_point start, clock::duration frameTime, bool occludedNow) {
		while (!this->mStop && this->mOccluded.load() == occludedNow) {
			const auto elapsed = clock::now() - start;
			if (elapsed >= frameTime) break;
			auto slice = frameTime - elapsed;
			if (slice > PACE_SLICE) slice = PACE_SLICE;
			std::this_thread::sleep_for(slice);
		}
	};

	while (!this->mStop) {
		auto start = clock::now();
		// Sampled once, and the whole iteration then agrees with itself: a flag
		// that flips mid-frame must not pace at one rate and publish at the other.
		const bool occludedNow = this->mOccluded.load();
		const auto frameTime =
		    std::chrono::milliseconds(1000 / (occludedNow ? OCCLUDED_FPS : this->mFps.load()));

		// Feed the shell's occlusion verdict into WE's own pause machinery
		// (qs-wallpaperengine#19). This is the half our occluded branch below
		// cannot cover: skipping the blit/publish stops everything Qt-side, but
		// mpv kept decoding - measured at a constant 180% CPU for a 7680x2160
		// software-decoded video under a fullscreen game. The patched
		// setExternalPaused() is ORed into render()'s fullscreen-detector
		// checks, so WE pauses exactly as it does standalone: RenderContext::
		// setPause -> CVideo -> GLPlayer -> mpv `pause` (decode stops), with
		// WE's own playlist-timer accounting and resume path. Same thread as
		// render(); takes effect on this iteration's render() call.
		app->setExternalPaused(occludedNow);
		// app->render() advances g_Time (driver clock - else the scene freezes at
		// t=0), updates audio/media, and drives the per-frame render + frame
		// counter. WE renders scenes into the wallpaper's OWN scene FBO
		// (getFirstWallpaperFramebuffer); the setDestinationFramebuffer composite
		// only works for some types (video), so blit the scene FBO into our
		// double-buffered target - reliable for both scene and video.
		//
		// While externally paused, render() early-returns after a 250us sleep
		// instead: WE stops advancing its clock, exactly as upstream's own
		// fullscreen pause does, and the resume path recomputes time and
		// playlist deadlines itself.
		app->render();
		if (occludedNow) {
			// The shell says a fullscreen window covers this output, so there is
			// nobody to publish to: skip the blit, the fence, the flush, the
			// publish and the wake-up - the entire cost the rest of the system
			// pays for a frame, up to and including the surface's repaint and the
			// window's wl_surface commit - and let the pacing above run WE at a
			// few hertz instead.
			//
			// app->render() keeps being called: it is what runs WE's paused
			// branch (and, on the iteration after the shell uncovers this
			// output, the resume). The decode half is handled above via
			// setExternalPaused; what remains here is only the publish half.
			//
			// Skipping the publish also skips `back ^= 1` and leaves mReady alone,
			// which is what keeps the rest of the loop's invariants true: back
			// still names the target that is not the front one, and a wallpaper
			// that loads onto an already-covered output reports "nothing ready"
			// rather than handing the consumer a texture nothing has drawn into
			// yet.
			pace(start, frameTime, occludedNow);
			continue;
		}

		auto& tgt = targets[back];
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

		// Publish the content size for the crop picker: the scene FBO's own
		// dimensions when there is one, 0 for a video (WE composited it at
		// screen size, so there is no distinct content aspect to pan).
		this->mContentW.store(sceneValid ? srcW : 0);
		this->mContentH.store(sceneValid ? srcH : 0);

		// A pending full-scene grab: read the uncropped scene FBO and write a
		// PNG at the real content aspect. One-shot, on the render thread that
		// owns the context. Only for a scene - a video has no scene FBO to
		// grab the full frame from.
		if (this->mGrabPending.load() && sceneValid) {
			this->mGrabPending.store(false);
			std::string path;
			std::function<void(std::string)> onGrab;
			{
				std::lock_guard<std::mutex> lock(this->mGrabMutex);
				path = this->mGrabPath;
				onGrab = this->mOnGrab;
			}
			std::vector<unsigned char> pixels(static_cast<std::size_t>(srcW) * srcH * 4);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFb);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(0, 0, srcW, srcH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
			// GL's origin is bottom-left; QImage's is top-left. Construct from
			// the buffer and mirror vertically before saving.
			QImage image(pixels.data(), srcW, srcH, QImage::Format_RGBA8888);
			if (!path.empty() && image.mirrored(false, true).save(QString::fromStdString(path), "PNG")) {
				if (onGrab) onGrab(path);
			}
		}

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
			} else { // "fill"/"default": cover, crop to the focus point
				// The crop window is the largest sub-rect of the source with
				// the output's aspect; where it sits along the overflowing
				// axis is the focus fraction (0.5 = centre, the old fixed
				// behaviour). Read live, so a drag pans without a reload.
				const float focusX = this->mFocusX.load();
				const float focusY = this->mFocusY.load();
				if (srcA > dstA) {
					const int w = static_cast<int>(srcH * dstA);
					sx0 = static_cast<int>((srcW - w) * focusX);
					sx1 = sx0 + w;
				} else {
					const int h = static_cast<int>(srcW / dstA);
					// GL's framebuffer origin is bottom-left, so a focus of 0
					// (the TOP of the picture, in the shell's top-left frame)
					// is the HIGH y here - invert so dragging up shows the top.
					sy0 = static_cast<int>((srcH - h) * (1.0f - focusY));
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

		pace(start, frameTime, occludedNow);
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
