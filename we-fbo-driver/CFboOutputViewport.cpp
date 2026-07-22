#include "CFboOutputViewport.h"

// TODO(embed): #include <GLES3/gl3.h> (or GL/glew.h to match WE's includes)

namespace WallpaperEngine::Render::Drivers::Output {

CFboOutputViewport::CFboOutputViewport(glm::ivec4 viewport, std::string name, unsigned int fbo)
    : OutputViewport(viewport, std::move(name), /*single=*/true), mFbo(fbo) {}

void CFboOutputViewport::makeCurrent() {
	// The host (Quickshell) already made the shared GL context current before
	// driving a frame; we only need to target our FBO.
	// TODO(embed):
	//   glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
	//   glViewport(this->viewport.x, this->viewport.y, this->viewport.z, this->viewport.w);
}

void CFboOutputViewport::swapOutput() {
	// No window/surface to swap; the FBO color texture is the output.
	// TODO(embed): glFlush();
}

} // namespace WallpaperEngine::Render::Drivers::Output
