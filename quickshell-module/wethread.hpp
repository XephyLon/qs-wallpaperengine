#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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
	    std::function<void()> onFrame = {}
	);
	~WeThread();

	// Called on Qt's render thread. Returns the GL texture id of the latest
	// completed frame (valid in the shared context), or 0 if none ready. Inserts
	// a wait on the producer's fence so sampling is safe.
	unsigned int acquireTexture();

	// Live-adjust the producer frame rate (thread-safe).
	void setFps(int fps) { this->mFps.store(fps > 0 ? fps : 60); }

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

	// True once ~WeThread has given up waiting for this thread and DETACHED it.
	// A detached thread is still running, still has its EGLContext current, and
	// still issues GL through it, so whoever owns that context must not destroy
	// it. Handed out as a shared_ptr because the answer is only known after the
	// destructor has run, i.e. when this object no longer exists - the caller
	// takes a copy BEFORE destroying the thread and reads it after.
	[[nodiscard]] std::shared_ptr<std::atomic<bool>> detachedFlag() const {
		return this->mDetached;
	}

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
	std::function<void()> mOnFrame;

	std::thread mThread;
	std::atomic<bool> mStop {false};
	std::atomic<int> mFps {60};
	std::atomic<bool> mFailed {false};
	std::shared_ptr<std::atomic<bool>> mDetached =
	    std::make_shared<std::atomic<bool>>(false);

	// Double buffer: producer draws mBack, publishes to mFront under mMutex.
	std::mutex mMutex;
	unsigned int mFrontTexture = 0; // last completed (consumer reads)
	void* mFrontFence = nullptr;    // EGLSync signalled when mFront is done
	std::atomic<bool> mReady {false};
};

} // namespace qs::wallpaperengine
