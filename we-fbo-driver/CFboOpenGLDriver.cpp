#include "CFboOpenGLDriver.h"

#include <EGL/egl.h>
#include <GL/glew.h>
#include <chrono>

namespace WallpaperEngine::Render::Drivers {

// VideoDriver's ctor stores the MouseInput& (via InputContext) without calling
// it, so passing our not-yet-constructed member is safe - the same pattern
// GLFWOpenGLDriver uses with its m_mouseInput.
//
// shareDisplay/shareContext are unused in the adopt-Qt-context path (kept for
// the fallback shared-context design); WE renders on whatever GL context is
// current when the host drives a frame.
CFboOpenGLDriver::CFboOpenGLDriver(
    ApplicationContext& context,
    WallpaperApplication& app,
    void* shareDisplay,
    void* shareContext,
    glm::ivec2 size
)
    : VideoDriver(app, m_mouseInput)
    , m_context(context)
    , m_shareDisplay(shareDisplay)
    , m_shareContext(shareContext) {
	// WE's GL calls go through glew's global function pointers; initialize them
	// against the current (host) context. Harmless if already initialized.
	glewExperimental = GL_TRUE;
	glewInit();
	this->m_output = new Output::CFboWindowOutput(context, *this, size);
}

CFboOpenGLDriver::~CFboOpenGLDriver() { delete this->m_output; }

float CFboOpenGLDriver::getRenderTime() const {
	using namespace std::chrono;
	static const auto start = steady_clock::now();
	return duration<float>(steady_clock::now() - start).count();
}

glm::ivec2 CFboOpenGLDriver::getFramebufferSize() const {
	return {this->m_output->getFullWidth(), this->m_output->getFullHeight()};
}

void CFboOpenGLDriver::resizeWindow(glm::ivec2 size) { this->m_output->resize(size); }
void CFboOpenGLDriver::resizeWindow(glm::ivec4 sizeAndPos) {
	this->m_output->resize({sizeAndPos.z, sizeAndPos.w});
}

void* CFboOpenGLDriver::getProcAddress(const char* name) const {
	return reinterpret_cast<void*>(eglGetProcAddress(name));
}

// Per-frame render, distilled from GLFWOpenGLDriver::dispatchEventQueue(): clear
// + let the app render each viewport. Deliberately omits glfwSwapBuffers /
// glfwPollEvents / the FPS usleep - the host (Quickshell) owns present + pacing,
// and there is no window/input to service. WE composites the final image into
// the FBO the host set via WallpaperApplication::setDestinationFramebuffer().
void CFboOpenGLDriver::dispatchEventQueue() {
	for (const auto& [screen, viewport] : this->m_output->getViewports()) {
		viewport->makeCurrent(); // bind FBO + glViewport
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		this->getApp().update(viewport); // WE renders the wallpaper
	}
	this->m_output->updateRender(); // no-op for the FBO output
	this->m_frameCounter++;
	// After this returns, texture() holds the frame for the host to sample.
}

} // namespace WallpaperEngine::Render::Drivers
