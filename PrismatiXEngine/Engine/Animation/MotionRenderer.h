#pragma once

#include "Engine/Core/Types.h"
#include "Engine/SDK/Motion.h"

namespace px::graphics { class Renderer2D; }

namespace px::animation {

// Draws an already sampled motion pose through the ordinary image cache and
// transform stack. The clip's fit mode preserves aspect ratio by default.
// Callers must preload MotionDependencies before presenting a transition.
void DrawMotionPose(graphics::Renderer2D& renderer, const sdk::MotionClip& clip,
                    const sdk::MotionSampler& sampler, Rect bounds);

}  // namespace px::animation
