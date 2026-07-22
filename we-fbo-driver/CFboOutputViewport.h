#pragma once

#include "WallpaperEngine/Render/Drivers/Output/OutputViewport.h"

namespace WallpaperEngine::Render::Drivers::Output {

// A viewport whose target is an offscreen FBO (not a window/surface). makeCurrent
// binds the FBO and sets the GL viewport; there is nothing to swap.
class CFboOutputViewport final : public OutputViewport {
public:
	// fbo points at the owning output's live FBO id (it changes on resize), so
	// makeCurrent always binds the current framebuffer.
	CFboOutputViewport(glm::ivec4 viewport, std::string name, const unsigned int* fbo);

	void makeCurrent() override; // glBindFramebuffer(*mFbo) + glViewport(viewport)
	void swapOutput() override;  // glFlush() — the texture is presented by the host

private:
	const unsigned int* mFbo;
};

} // namespace WallpaperEngine::Render::Drivers::Output
