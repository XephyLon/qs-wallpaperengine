#pragma once

#include "WallpaperEngine/Input/MouseInput.h"

namespace WallpaperEngine::Input {

// No-op mouse input for headless/offscreen rendering (the FBO driver has no
// window and no pointer). Satisfies VideoDriver's ctor requirement.
class NullMouseInput final : public MouseInput {
public:
	void update() override {}
	[[nodiscard]] glm::dvec2 position() const override { return {0.0, 0.0}; }
	[[nodiscard]] MouseClickStatus leftClick() const override { return Released; }
	[[nodiscard]] MouseClickStatus rightClick() const override { return Released; }
};

} // namespace WallpaperEngine::Input
