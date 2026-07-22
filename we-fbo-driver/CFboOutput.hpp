#pragma once

// Renders a Wallpaper Engine wallpaper into an offscreen FBO whose color texture
// is shared with a host GL context (Quickshell's). A sibling of WE's
// WaylandOpenGLDriver / GLFWOpenGLDriver.
//
// NOTE: base classes + exact virtual signatures are placeholders. Reconcile
// against the real headers once the from-source WE build materializes them
// (WallpaperEngine/Render/Drivers/CVideoDriver.h, .../Output/COutputViewport.h,
// .../CRenderContext.h). Grep those for the pure-virtuals CFboOutput must
// override, then fill the TODOs.

#include <cstdint>

// TODO(embed): #include "WallpaperEngine/Render/Drivers/CVideoDriver.h"
//              #include "WallpaperEngine/Render/Drivers/Output/COutput.h"

namespace WallpaperEngine::Render::Drivers {

// One viewport covering the whole wallpaper, targeting our FBO.
class CFboOutputViewport /* : public Output::COutputViewport */ {
public:
	CFboOutputViewport(int width, int height);

	// [[nodiscard]] const glm::ivec4& getViewport() const override; // {0,0,w,h}
	// bool renderVFlip() const override;  // FBO textures are bottom-up for GL

	[[nodiscard]] int width() const { return this->mWidth; }
	[[nodiscard]] int height() const { return this->mHeight; }

private:
	int mWidth;
	int mHeight;
};

// The output driver. Adopts a GL/EGL context that shares objects with the host,
// owns the FBO, and drives the wallpaper into it one frame at a time.
class CFboOutput /* : public CVideoDriver */ {
public:
	// hostShareContext: the EGLContext to share GL objects with (Quickshell's),
	// so texture() is valid in the host context. hostDisplay: its EGLDisplay.
	CFboOutput(void* hostDisplay, void* hostShareContext);
	~CFboOutput();

	// Allocate/resize the FBO + color texture to the wallpaper size.
	void resize(int width, int height);

	// Make the WE context current, render the active wallpaper into the FBO,
	// flush. Call with a valid GL context on the host render thread. The frame
	// is driven by RenderContext::render(&viewport()) from the caller.
	void present();

	// GL texture id of the FBO color attachment. Valid in the shared/host
	// context after present().
	[[nodiscard]] std::uint32_t texture() const { return this->mColorTexture; }

	[[nodiscard]] CFboOutputViewport& viewport() { return this->mViewport; }

	// --- CVideoDriver overrides (fill signatures from real header) -----------
	// void* getProcAddress(const char* name) override;
	// std::shared_ptr<InputContext> getInputContext() override; // no input
	// bool closeRequested() override { return false; }
	// void resetInput() override {}
	// [[nodiscard]] uint32_t getFrameCounter() const override;
	// void dispatchEventQueue() override;   // no-op: we're pull-driven
	// -------------------------------------------------------------------------

private:
	void ensureContext();
	void ensureFbo();

	void* mHostDisplay = nullptr;       // EGLDisplay
	void* mHostShareContext = nullptr;  // EGLContext to share with
	void* mContext = nullptr;           // our EGLContext (shares with host)

	std::uint32_t mFbo = 0;
	std::uint32_t mColorTexture = 0;
	std::uint32_t mDepthRbo = 0;
	std::uint32_t mFrameCounter = 0;

	CFboOutputViewport mViewport {0, 0};
};

} // namespace WallpaperEngine::Render::Drivers
