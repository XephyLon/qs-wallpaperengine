#include "CFboOpenGLDriver.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glew.h>
#include <chrono>
#include <cstdio>

namespace WallpaperEngine::Render::Drivers {

// Grab Qt's EGLConfig from its current context so our shared context is
// pixel-format-compatible.
static EGLConfig configOfCurrentContext(EGLDisplay dpy, EGLContext ctx) {
	EGLint cfgId = 0;
	if (!eglQueryContext(dpy, ctx, EGL_CONFIG_ID, &cfgId)) return nullptr;
	EGLint attr[] = {EGL_CONFIG_ID, cfgId, EGL_NONE};
	EGLConfig cfg = nullptr;
	EGLint n = 0;
	eglChooseConfig(dpy, attr, &cfg, 1, &n);
	return n > 0 ? cfg : nullptr;
}

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
	// We are constructed during WallpaperApplication::setup(), which the host
	// (Quickshell) calls with Qt's GL/EGL context current on the render thread.
	// Grab it and create our own context that SHARES GL objects with it, so WE
	// renders entirely on our context (Qt's state untouched) yet the FBO texture
	// stays valid in Qt's context.
	auto dpy = eglGetCurrentDisplay();
	auto qtCtx = eglGetCurrentContext();
	this->m_display = dpy;

	if (dpy != EGL_NO_DISPLAY && qtCtx != EGL_NO_CONTEXT) {
		auto cfg = configOfCurrentContext(dpy, qtCtx);
		eglBindAPI(EGL_OPENGL_API); // WE is desktop GL (glew)
		EGLint ctxAttr[] = {
		    EGL_CONTEXT_MAJOR_VERSION, 3,
		    EGL_CONTEXT_MINOR_VERSION, 3,
		    EGL_NONE,
		};
		this->m_weContext = eglCreateContext(dpy, cfg, qtCtx, ctxAttr);
		if (this->m_weContext == EGL_NO_CONTEXT) {
			std::fprintf(stderr, "CFboOpenGLDriver: eglCreateContext failed: 0x%x\n", eglGetError());
		}
	}

	// Build the FBO on OUR context so its texture belongs to the shared group.
	this->makeCurrentWe();
	glewExperimental = GL_TRUE;
	glewInit();
	this->m_output = new Output::CFboWindowOutput(context, *this, size);
	this->restoreHost();
}

CFboOpenGLDriver::~CFboOpenGLDriver() {
	// Destroy the output's GL objects on our context.
	if (this->m_weContext) {
		this->makeCurrentWe();
		delete this->m_output;
		this->m_output = nullptr;
		this->restoreHost();
		eglDestroyContext(static_cast<EGLDisplay>(this->m_display), static_cast<EGLContext>(this->m_weContext));
		this->m_weContext = nullptr;
	} else {
		delete this->m_output;
	}
}

void CFboOpenGLDriver::makeCurrentWe() {
	if (!this->m_weContext) return;
	auto dpy = static_cast<EGLDisplay>(this->m_display);
	this->m_savedContext = eglGetCurrentContext();
	this->m_savedDrawSurface = eglGetCurrentSurface(EGL_DRAW);
	this->m_savedReadSurface = eglGetCurrentSurface(EGL_READ);
	// Surfaceless: we only render to an FBO (needs EGL_KHR_surfaceless_context,
	// present on Mesa).
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, static_cast<EGLContext>(this->m_weContext));
}

void CFboOpenGLDriver::restoreHost() {
	if (!this->m_weContext) return;
	auto dpy = static_cast<EGLDisplay>(this->m_display);
	eglMakeCurrent(
	    dpy,
	    static_cast<EGLSurface>(this->m_savedDrawSurface),
	    static_cast<EGLSurface>(this->m_savedReadSurface),
	    static_cast<EGLContext>(this->m_savedContext)
	);
}

float CFboOpenGLDriver::getRenderTime() const {
	using namespace std::chrono;
	static const auto start = steady_clock::now();
	return duration<float>(steady_clock::now() - start).count();
}

glm::ivec2 CFboOpenGLDriver::getFramebufferSize() const {
	return {this->m_output->getFullWidth(), this->m_output->getFullHeight()};
}

void CFboOpenGLDriver::resizeWindow(glm::ivec2 size) {
	this->makeCurrentWe();
	this->m_output->resize(size);
	this->restoreHost();
}
void CFboOpenGLDriver::resizeWindow(glm::ivec4 sizeAndPos) {
	this->resizeWindow(glm::ivec2 {sizeAndPos.z, sizeAndPos.w});
}

void* CFboOpenGLDriver::getProcAddress(const char* name) const {
	return reinterpret_cast<void*>(eglGetProcAddress(name));
}

// Per-frame render, on OUR context. Distilled from
// GLFWOpenGLDriver::dispatchEventQueue() minus swap/poll/usleep. WE composites
// the final image into the FBO the host set via setDestinationFramebuffer().
void CFboOpenGLDriver::dispatchEventQueue() {
	this->makeCurrentWe();
	for (const auto& [screen, viewport] : this->m_output->getViewports()) {
		viewport->makeCurrent(); // bind FBO + glViewport
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		this->getApp().update(viewport); // WE renders the wallpaper
	}
	this->m_output->updateRender();
	glFlush(); // make the frame visible to Qt's context (shared)
	this->restoreHost();
	this->m_frameCounter++;
}

} // namespace WallpaperEngine::Render::Drivers
