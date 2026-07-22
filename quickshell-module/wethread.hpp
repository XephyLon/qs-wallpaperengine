#pragma once

#include <atomic>
#include <condition_variable>
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
	WeThread(
	    void* display,
	    void* sharedContext,
	    std::string projectPath,
	    std::string assetsDir,
	    int width,
	    int height,
	    int fps
	);
	~WeThread();

	// Called on Qt's render thread. Returns the GL texture id of the latest
	// completed frame (valid in the shared context), or 0 if none ready. Inserts
	// a wait on the producer's fence so sampling is safe.
	unsigned int acquireTexture();

	// Live-adjust the producer frame rate (thread-safe).
	void setFps(int fps) { this->mFps.store(fps > 0 ? fps : 60); }

	[[nodiscard]] int width() const { return this->mWidth; }
	[[nodiscard]] int height() const { return this->mHeight; }

private:
	void run(); // thread body

	void* mDisplay;
	void* mContext;
	std::string mProjectPath;
	std::string mAssetsDir;
	int mWidth;
	int mHeight;

	std::thread mThread;
	std::atomic<bool> mStop {false};
	std::atomic<int> mFps {60};

	// Double buffer: producer draws mBack, publishes to mFront under mMutex.
	std::mutex mMutex;
	unsigned int mFrontTexture = 0; // last completed (consumer reads)
	void* mFrontFence = nullptr;    // EGLSync signalled when mFront is done
	std::atomic<bool> mReady {false};
};

} // namespace qs::wallpaperengine
