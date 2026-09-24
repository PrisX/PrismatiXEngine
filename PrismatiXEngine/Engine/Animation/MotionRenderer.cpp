#include "Engine/Animation/MotionRenderer.h"

#include "Engine/Graphics/Renderer2D.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace px::animation {

void DrawMotionPose(graphics::Renderer2D& renderer, const sdk::MotionClip& clip,
                    const sdk::MotionSampler& sampler, const Rect bounds) {
    if (bounds.w <= 0 || bounds.h <= 0 || clip.canvasWidth <= 0 || clip.canvasHeight <= 0)
        return;
    double scaleX = static_cast<double>(bounds.w) / clip.canvasWidth;
    double scaleY = static_cast<double>(bounds.h) / clip.canvasHeight;
    if (clip.scaleMode != sdk::MotionScaleMode::Stretch) {
        const double uniform = clip.scaleMode == sdk::MotionScaleMode::Fit
            ? std::min(scaleX, scaleY) : std::max(scaleX, scaleY);
        scaleX = uniform;
        scaleY = uniform;
    }
    const double offsetX = bounds.x + (bounds.w - clip.canvasWidth * scaleX) * 0.5;
    const double offsetY = bounds.y + (bounds.h - clip.canvasHeight * scaleY) * 0.5;
    renderer.PushClip(bounds);
    renderer.PushAffineTransform({static_cast<float>(scaleX), 0, 0,
        static_cast<float>(scaleY), static_cast<float>(offsetX), static_cast<float>(offsetY)});
    for (const auto& draw : sampler.Pose()) {
        if (!draw.visible || draw.opacity <= 0 || draw.layer >= clip.layers.size()) continue;
        const auto& layer = clip.layers[draw.layer];
        const auto& transform = draw.transform;
        const auto representable = [](double value) {
            return std::isfinite(value) &&
                std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max());
        };
        if (!representable(transform.a) || !representable(transform.b) ||
            !representable(transform.c) || !representable(transform.d) ||
            !representable(transform.tx) || !representable(transform.ty) ||
            !std::isfinite(draw.opacity)) continue;
        renderer.PushAffineTransform({static_cast<float>(transform.a),
            static_cast<float>(transform.b), static_cast<float>(transform.c),
            static_cast<float>(transform.d), static_cast<float>(transform.tx),
            static_cast<float>(transform.ty)});
        const auto alpha = static_cast<std::uint8_t>(std::clamp(
            std::lround(draw.opacity * 255), 0l, 255l));
        renderer.DrawImage(layer.image, {0, 0, static_cast<float>(layer.width),
                           static_cast<float>(layer.height)}, alpha);
        renderer.PopTransform();
    }
    renderer.PopTransform();
    renderer.PopClip();
}

}  // namespace px::animation
