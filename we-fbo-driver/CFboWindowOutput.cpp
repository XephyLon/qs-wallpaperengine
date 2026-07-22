#include "CFboWindowOutput.h"

#include "CFboOutputViewport.h"
#include <GL/glew.h>

namespace WallpaperEngine::Render::Drivers::Output {

CFboWindowOutput::CFboWindowOutput(ApplicationContext& context, VideoDriver& driver, glm::ivec2 size)
    : Output(context, driver) {
	this->ensureFbo(size);
	this->m_fullWidth = size.x;
	this->m_fullHeight = size.y;
	// Single viewport covering the whole FBO. Owned here; base's map is not the
	// owner. The viewport reads &mFbo so it survives resizes.
	this->m_viewports["fbo"] =
	    new CFboOutputViewport({0, 0, size.x, size.y}, "fbo", &this->mFbo);
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
	this->ensureFbo(size); // recreates mFbo/mTexture; viewport reads &mFbo
	this->m_fullWidth = size.x;
	this->m_fullHeight = size.y;
	for (auto& [name, viewport] : this->m_viewports) viewport->viewport = {0, 0, size.x, size.y};
}

void CFboWindowOutput::ensureFbo(glm::ivec2 size) {
	this->destroyFbo();
	if (size.x <= 0 || size.y <= 0) return;

	glGenTextures(1, &this->mTexture);
	glBindTexture(GL_TEXTURE_2D, this->mTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenRenderbuffers(1, &this->mDepthStencil);
	glBindRenderbuffer(GL_RENDERBUFFER, this->mDepthStencil);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, size.x, size.y);

	glGenFramebuffers(1, &this->mFbo);
	glBindFramebuffer(GL_FRAMEBUFFER, this->mFbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->mTexture, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, this->mDepthStencil);

	// Best-effort: leave the default framebuffer bound so we don't disturb Qt.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void CFboWindowOutput::destroyFbo() {
	if (this->mFbo) glDeleteFramebuffers(1, &this->mFbo);
	if (this->mTexture) glDeleteTextures(1, &this->mTexture);
	if (this->mDepthStencil) glDeleteRenderbuffers(1, &this->mDepthStencil);
	this->mFbo = this->mTexture = this->mDepthStencil = 0;
}

} // namespace WallpaperEngine::Render::Drivers::Output
