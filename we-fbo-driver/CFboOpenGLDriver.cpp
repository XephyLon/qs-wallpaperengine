#include "CFboOpenGLDriver.h"

// TODO(embed): #include <EGL/egl.h> <GLES3/gl3.h>

namespace WallpaperEngine::Render::Drivers {

// NOTE: VideoDriver's ctor is VideoDriver(WallpaperApplication&, Input::MouseInput&).
// Passing the mouse input requires a concrete instance (see header TODO); shown
// here as `nullMouseInput` to be resolved against Input/MouseInput.h.
CFboOpenGLDriver::CFboOpenGLDriver(
    ApplicationContext& context,
    WallpaperApplication& app,
    void* shareDisplay,
    void* shareContext,
    glm::ivec2 size
)
    : VideoDriver(app, /*TODO nullMouseInput*/ *static_cast<Input::MouseInput*>(nullptr))
    , m_context(context)
    , m_shareDisplay(shareDisplay)
    , m_shareContext(shareContext) {
	// TODO(embed): create an EGLContext on m_shareDisplay with m_shareContext as
	// share_context (compatible config, GLES2/3). The host makes it - or its own
	// context that shares with it - current before dispatchEventQueue().
	this->m_output = new Output::CFboWindowOutput(context, *this, size);
}

CFboOpenGLDriver::~CFboOpenGLDriver() {
	delete this->m_output;
	// TODO(embed): eglDestroyContext(...)
}

float CFboOpenGLDriver::getRenderTime() const {
	// TODO(embed): monotonic seconds since start (WE uses this for animation).
	// Prefer feeding the host's frame clock so WE animates in step with Qt.
	return 0.0f;
}

glm::ivec2 CFboOpenGLDriver::getFramebufferSize() const {
	return {this->m_output->getFullWidth(), this->m_output->getFullHeight()};
}

void CFboOpenGLDriver::resizeWindow(glm::ivec2 size) { this->m_output->resize(size); }
void CFboOpenGLDriver::resizeWindow(glm::ivec4 sizeAndPos) {
	this->m_output->resize({sizeAndPos.z, sizeAndPos.w});
}

void* CFboOpenGLDriver::getProcAddress(const char* name) const {
	// TODO(embed): return reinterpret_cast<void*>(eglGetProcAddress(name));
	(void) name;
	return nullptr;
}

// The per-frame render, distilled from GLFWOpenGLDriver::dispatchEventQueue():
// clear + let the app render each viewport into our FBO. Deliberately omits
// glfwSwapBuffers / glfwPollEvents / usleep — the host (Quickshell) owns
// presentation and frame pacing, and there is no window/input to service.
void CFboOpenGLDriver::dispatchEventQueue() {
	// Host has the shared GL context current on its render thread.
	for (const auto& [screen, viewport] : this->m_output->getViewports()) {
		viewport->makeCurrent(); // bind FBO + set glViewport
		// TODO(embed): glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		this->getApp().update(viewport); // WE renders the wallpaper into the FBO
	}
	this->m_output->updateRender(); // no-op for the FBO output
	this->m_frameCounter++;
	// After this returns, texture() holds the frame for the host to sample.
}

} // namespace WallpaperEngine::Render::Drivers
