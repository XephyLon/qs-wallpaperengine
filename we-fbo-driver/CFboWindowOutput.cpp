#include "CFboWindowOutput.h"

#include "CFboOutputViewport.h"

// TODO(embed): #include <GLES3/gl3.h>

namespace WallpaperEngine::Render::Drivers::Output {

CFboWindowOutput::CFboWindowOutput(ApplicationContext& context, VideoDriver& driver, glm::ivec2 size)
    : Output(context, driver) {
	this->ensureFbo(size);
	this->m_fullWidth = size.x;
	this->m_fullHeight = size.y;
	// Single viewport covering the whole FBO. Owned by m_viewports (base clears).
	this->m_viewports["fbo"] =
	    new CFboOutputViewport({0, 0, size.x, size.y}, "fbo", this->mFbo);
}

CFboWindowOutput::~CFboWindowOutput() {
	for (auto& [name, viewport] : this->m_viewports) delete viewport;
	this->m_viewports.clear();
	this->destroyFbo();
}

void CFboWindowOutput::reset() {
	// Nothing to reposition/reconfigure for an offscreen FBO.
}

void CFboWindowOutput::resize(glm::ivec2 size) {
	if (size.x == this->m_fullWidth && size.y == this->m_fullHeight) return;
	this->ensureFbo(size);
	this->m_fullWidth = size.x;
	this->m_fullHeight = size.y;
	for (auto& [name, viewport] : this->m_viewports) viewport->viewport = {0, 0, size.x, size.y};
}

void CFboWindowOutput::ensureFbo(glm::ivec2 /*size*/) {
	this->destroyFbo();
	// TODO(embed):
	//   glGenTextures(1,&mTexture); bind; glTexImage2D(GL_RGBA8, size.x, size.y, ...)
	//   glGenRenderbuffers(1,&mDepthStencil); glRenderbufferStorage(GL_DEPTH24_STENCIL8, size)
	//   glGenFramebuffers(1,&mFbo); attach GL_COLOR_ATTACHMENT0 = mTexture,
	//     GL_DEPTH_STENCIL_ATTACHMENT = mDepthStencil
	//   assert glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE
	// Recreate the viewport's mFbo reference after (re)allocation.
}

void CFboWindowOutput::destroyFbo() {
	// TODO(embed): glDeleteFramebuffers/Textures/Renderbuffers if nonzero.
	this->mFbo = this->mTexture = this->mDepthStencil = 0;
}

} // namespace WallpaperEngine::Render::Drivers::Output
