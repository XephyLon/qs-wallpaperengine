#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Runs a linux-wallpaperengine wallpaper on its OWN thread with its OWN EGL
// context (sharing GL objects with Qt's, so the output texture is usable by Qt).
// This keeps ALL of WE's GL/EGL off Qt's GUI + render threads, which is required
// on NVIDIA where WE's GL on Qt's threads corrupts Qt's EGL/Wayland dispatch.
//
// Producer/consumer: WE renders into one of two textures; the surface samples
// the other. A GL fence makes sure Qt never reads a half-written frame.
namespace qs::wallpaperengine {

class WeThread {
public:
	// display: Qt's EGLDisplay. sharedContext: an EGLContext already created to
	// share with Qt's (built via QOpenGLContext::setShareContext so NVIDIA's
	// share-compatibility flags match), not current on any thread. The WE thread
	// makes it current surfacelessly and owns it for its lifetime.
	//
	// onFrame is called ON THIS THREAD every time a new frame is published, and
	// once more if the wallpaper fails to start. It is how the consumer learns
	// there is something new to draw: the surface does NOT poll on a timer, so a
	// producer that publishes at 24fps makes the surface repaint 24 times a
	// second - no duplicate repaints of a frame that has not changed, and no
	// missed frames from a poll clock that drifts against this one. It must be
	// cheap and thread-safe (it runs inside the render loop); the surface's one
	// posts a queued call and returns.
	// volume is in WE's own units (0..128, its --volume argument); the surface
	// maps its 0..100 QML property before constructing this. audioProcessing,
	// the three disable flags and setProperties are all load-time WE arguments
	// exactly like scaleMode/audioEnabled: the surface rebuilds this thread to
	// change any of them. setProperties entries are already in --set-property's
	// own "name=value" form; this class does not interpret them.
	WeThread(
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
	    std::function<void()> onFrame = {}
	);
	~WeThread();

	// Stop the render thread and wait, BOUNDED, for it to finish. Returns true
	// if it actually finished: the object is then quiescent and may be destroyed.
	// Returns false if the wait timed out and the thread had to be DETACHED - it
	// is then still executing run(), which is a MEMBER function: it reads
	// mStop/mFps/mScaleMode, locks mMutex, publishes into mFrontTexture and
	// mFrontFence, and notifies through mOnFrame. After a false this object must
	// NEVER be destroyed or freed - leak it, exactly as its GL context already
	// is (see WallpaperEngineSurface::releaseThread).
	//
	// Idempotent: once the thread has been joined or detached, later calls
	// report the same verdict without waiting again.
	//
	// Deliberately not left to the destructor: the verdict is only known from
	// inside the bounded join, by which point ~WeThread has already committed to
	// destroying the very members run() is still using.
	[[nodiscard]] bool stop();

	// Called on Qt's render thread. Returns the GL texture id of the latest
	// completed frame (valid in the shared context), or 0 if none ready. Inserts
	// a wait on the producer's fence so sampling is safe.
	unsigned int acquireTexture();

	// Live-adjust the producer frame rate (thread-safe).
	void setFps(int fps) { this->mFps.store(fps > 0 ? fps : 60); }

	// The shell has decided a fullscreen window covers the output this wallpaper
	// is drawn on (thread-safe, live - deliberately NOT a thread rebuild the way
	// scaleMode and audioEnabled are, because nothing about the load changes).
	// While it is set the loop keeps turning at a few hertz - enough that neither
	// WE's wall-clock time base nor mpv's render context is left with anything to
	// catch up on when the output comes back - but it publishes nothing and never
	// calls mOnFrame, so the consumer stops repainting and the wallpaper's
	// wl_surface stops committing.
	//
	// This is the shell's replacement for WE's own fullscreen pause, which the
	// embed disables (see the argv comment in run()). WE's detector counts every
	// fullscreen toplevel on every output through one flat counter, and the shell
	// runs one of these threads per output, so the shell is the only side that
	// knows which output is actually covered. It also reaches mpv: the render
	// loop forwards this flag into the patched
	// WallpaperApplication::setExternalPaused() each iteration, which runs WE's
	// own pause machinery and stops video decode (qs-wallpaperengine#19).
	void setOccluded(bool occluded) { this->mOccluded.store(occluded); }

	// This wallpaper cannot render and never will: the context could not be made
	// current, GL did not load, our render targets came back INCOMPLETE, or WE
	// threw out of setup(). Latched on the WE thread, polled from Qt's render
	// thread. The surface republishes it as a QML property so the shell can fall
	// back to the static image - the same graceful degradation web wallpapers
	// already get - instead of showing a permanently black desktop.
	//
	// Deliberately NOT driven by glGetError. WE never clears the GL error queue
	// (its render path calls glGetError in exactly one place, the screenshot
	// path), so a stale error survives from frame to frame as a matter of course:
	// every video wallpaper start on this machine logs
	// "[libmpv_render] after creating texture: OpenGL error
	// INVALID_FRAMEBUFFER_OPERATION" - mpv's own gl_check_error draining an
	// error WE left behind - and then plays perfectly well. Failing on a
	// non-empty error queue would black out wallpapers that work.
	[[nodiscard]] bool failed() const { return this->mFailed.load(); }

	[[nodiscard]] int width() const { return this->mWidth; }
	[[nodiscard]] int height() const { return this->mHeight; }

private:
	void run(); // thread body
	void notify() {
		if (this->mOnFrame) this->mOnFrame();
	}

	void* mDisplay;
	void* mContext;
	std::string mProjectPath;
	std::string mAssetsDir;
	std::string mScaleMode;
	int mWidth;
	int mHeight;
	bool mAudioEnabled;
	// Load-time arguments, read only by run()'s argv construction. mVolumeArg
	// is kept as the string argv needs, so its c_str() stays valid for WE's
	// whole run the way mScaleMode's does; mSetProperties likewise owns every
	// "name=value" string WE holds a pointer into.
	std::string mVolumeArg;
	bool mAudioProcessing;
	bool mMouseDisabled;
	bool mParallaxDisabled;
	bool mParticlesDisabled;
	std::vector<std::string> mSetProperties;
	std::function<void()> mOnFrame;

	std::thread mThread;
	std::atomic<bool> mStop {false};
	std::atomic<int> mFps {60};
	std::atomic<bool> mFailed {false};

	// Read by the render loop every iteration, written by the consumer through
	// setOccluded. Safe on the detach path for the same reason mStop and mFps
	// are: a detached thread's WeThread is leaked rather than freed, so this
	// member outlives every reader.
	std::atomic<bool> mOccluded {false};
	std::shared_ptr<std::atomic<bool>> mDetached =
	    std::make_shared<std::atomic<bool>>(false);

	// Double buffer: producer draws mBack, publishes to mFront under mMutex.
	std::mutex mMutex;
	unsigned int mFrontTexture = 0; // last completed (consumer reads)
	void* mFrontFence = nullptr;    // EGLSync signalled when mFront is done
	std::atomic<bool> mReady {false};
};

} // namespace qs::wallpaperengine
