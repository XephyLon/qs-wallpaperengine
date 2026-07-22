#pragma once

#include "WallpaperEngine/Render/Drivers/Output/Output.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"

namespace WallpaperEngine::Render::Drivers::Output {

// An Output backed by a single offscreen FBO. Its color attachment is a GL
// texture the host (Quickshell) imports as a QSGTexture — no CPU readback.
// Models GLFWWindowOutput but with an FBO instead of the GLFW window.
class CFboWindowOutput final : public Output {
public:
	CFboWindowOutput(ApplicationContext& context, VideoDriver& driver, glm::ivec2 size);
	~CFboWindowOutput() override;

	void reset() override;
	// GL FBO textures are bottom-up, so scenes must render V-flipped (same as
	// the on-screen GL drivers expect).
	bool renderVFlip() const override { return true; }
	bool renderMultiple() const override { return false; }
	// We hand out a GL texture, not a CPU pixel buffer, so no readback path.
	bool haveImageBuffer() const override { return false; }
	void* getImageBuffer() const override { return nullptr; }
	uint32_t getImageBufferSize() const override { return 0; }
	// Nothing to present: the FBO color texture is live for the host each frame.
	void updateRender() const override {}

	// Host-facing: the FBO color attachment texture id, valid in the shared context.
	[[nodiscard]] unsigned int texture() const { return this->mTexture; }
	void resize(glm::ivec2 size);

private:
	void ensureFbo(glm::ivec2 size);
	void destroyFbo();

	unsigned int mFbo = 0;
	unsigned int mTexture = 0;
	unsigned int mDepthStencil = 0;
};

} // namespace WallpaperEngine::Render::Drivers::Output
