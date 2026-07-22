#pragma once

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Input/NullMouseInput.h"
#include "WallpaperEngine/Render/Drivers/Output/CFboWindowOutput.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"
#include <glm/vec2.hpp>

namespace WallpaperEngine::Render::Drivers {
using namespace WallpaperEngine::Application;

// A VideoDriver that renders into an offscreen FBO on a GL context shared with a
// host (Quickshell). No window, no event loop, no buffer swap — the host drives
// one frame per its own render tick and samples the FBO texture.
//
// Models GLFWOpenGLDriver; the differences are all "no window": the context is
// adopted from the host (shared), dispatchEventQueue omits swap/poll/sleep, and
// the output is a CFboWindowOutput.
class CFboOpenGLDriver final : public VideoDriver {
public:
	// shareDisplay/shareContext: the host's EGLDisplay/EGLContext to share GL
	// objects with, so the FBO texture is valid in the host context.
	CFboOpenGLDriver(
	    ApplicationContext& context,
	    WallpaperApplication& app,
	    void* shareDisplay,
	    void* shareContext,
	    glm::ivec2 size
	);
	~CFboOpenGLDriver() override;

	[[nodiscard]] Output::Output& getOutput() override { return *this->m_output; }
	[[nodiscard]] float getRenderTime() const override;
	bool closeRequested() override { return false; }
	void resizeWindow(glm::ivec2 size) override;
	void resizeWindow(glm::ivec4 sizeAndPos) override;
	void showWindow() override {}
	void hideWindow() override {}
	[[nodiscard]] glm::ivec2 getFramebufferSize() const override;
	[[nodiscard]] uint32_t getFrameCounter() const override { return this->m_frameCounter; }
	void dispatchEventQueue() override; // renders one frame into the FBO
	[[nodiscard]] void* getProcAddress(const char* name) const override; // eglGetProcAddress

	// Host-facing: FBO color texture id (sample this) and FBO id (feed to
	// WallpaperApplication::setDestinationFramebuffer so WE composites into it).
	[[nodiscard]] unsigned int texture() const { return this->m_output->texture(); }
	[[nodiscard]] unsigned int fbo() const { return this->m_output->fbo(); }

private:
	// Make our WE EGL context current (surfaceless); saves the caller's (Qt's)
	// context+surfaces so restoreHost() can put them back. All WE GL runs here so
	// Qt's GL/RHI state is never touched.
	void makeCurrentWe();
	void restoreHost();

	ApplicationContext& m_context;
	Input::NullMouseInput m_mouseInput; // headless: no pointer
	Output::CFboWindowOutput* m_output = nullptr;

	// EGL handles kept as void* so the header stays EGL-free.
	void* m_display = nullptr;    // EGLDisplay (shared with Qt)
	void* m_weContext = nullptr;  // our EGLContext, shares GL objects with Qt's
	// Qt's current context/surfaces, saved across a WE frame.
	void* m_savedContext = nullptr;
	void* m_savedDrawSurface = nullptr;
	void* m_savedReadSurface = nullptr;

	void* m_shareDisplay = nullptr; // (unused; self-grabbed via eglGetCurrent*)
	void* m_shareContext = nullptr;
	uint32_t m_frameCounter = 0;
};

} // namespace WallpaperEngine::Render::Drivers
