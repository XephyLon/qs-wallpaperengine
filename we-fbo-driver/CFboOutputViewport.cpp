#include "CFboOutputViewport.h"

#include <GL/glew.h>

namespace WallpaperEngine::Render::Drivers::Output {

CFboOutputViewport::CFboOutputViewport(glm::ivec4 viewport, std::string name, const unsigned int* fbo)
    : OutputViewport(viewport, std::move(name), /*single=*/true), mFbo(fbo) {
	// The scaling UV math (CWallpaper) needs a non-zero logical size; for a
	// single offscreen FBO it is just the viewport size at origin.
	this->globalPosition = {viewport.x, viewport.y};
	this->logicalSize = {viewport.z, viewport.w};
}

void CFboOutputViewport::makeCurrent() {
	// Qt's GL context is already current on the render thread; we only retarget
	// to our FBO and set the region WE should draw into.
	glBindFramebuffer(GL_FRAMEBUFFER, *this->mFbo);
	glViewport(this->viewport.x, this->viewport.y, this->viewport.z, this->viewport.w);
}

void CFboOutputViewport::swapOutput() {
	// No window/surface to swap; the FBO color texture is the output.
	glFlush();
}

} // namespace WallpaperEngine::Render::Drivers::Output
