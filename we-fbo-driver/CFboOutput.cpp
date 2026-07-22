#include "CFboOutput.hpp"

// TODO(embed): #include <EGL/egl.h> <GLES3/gl3.h> and the WE headers.
// The bodies below are the intended control flow; the GL/EGL calls are sketched
// so the shape is reviewable before it compiles against real headers.

namespace WallpaperEngine::Render::Drivers {

CFboOutputViewport::CFboOutputViewport(int width, int height)
    : mWidth(width), mHeight(height) {}

CFboOutput::CFboOutput(void* hostDisplay, void* hostShareContext)
    : mHostDisplay(hostDisplay), mHostShareContext(hostShareContext) {
	this->ensureContext();
}

CFboOutput::~CFboOutput() {
	// TODO(embed): glDeleteFramebuffers(1,&mFbo); glDeleteTextures(1,&mColorTexture);
	//              glDeleteRenderbuffers(1,&mDepthRbo); eglDestroyContext(...);
}

void CFboOutput::ensureContext() {
	// TODO(embed): create an EGLContext on mHostDisplay with mHostShareContext as
	// the share context and a config compatible with the host. WE renders on
	// this (or directly on the host context on the render thread); sharing makes
	// mColorTexture usable in the host context regardless.
}

void CFboOutput::resize(int width, int height) {
	if (width == this->mViewport.width() && height == this->mViewport.height()) return;
	this->mViewport = CFboOutputViewport(width, height);
	this->ensureFbo();
}

void CFboOutput::ensureFbo() {
	// TODO(embed):
	//   glGenTextures(1,&mColorTexture); glBindTexture; glTexImage2D(RGBA8, w,h)
	//   glGenRenderbuffers(1,&mDepthRbo); glRenderbufferStorage(DEPTH24_STENCIL8)
	//   glGenFramebuffers(1,&mFbo); attach color0 + depth/stencil
	//   assert glCheckFramebufferStatus == COMPLETE
}

void CFboOutput::present() {
	// Called by the host with a GL context current. The caller runs
	// RenderContext::render(&viewport()) which binds scene FBOs and finally draws
	// into ours. Here we just make sure our FBO is bound + sized and flush.
	// TODO(embed):
	//   glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
	//   glViewport(0,0, viewport().width(), viewport().height());
	//   (RenderContext::render is invoked around this by the surface)
	//   glFlush();
	this->mFrameCounter++;
}

} // namespace WallpaperEngine::Render::Drivers
