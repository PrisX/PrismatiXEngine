#pragma once

#include "Engine/Core/Types.h"

namespace px::editor {

class CanvasTransform {
public:
    CanvasTransform() = default;
    CanvasTransform(Vec2 screenOrigin, float zoom) : m_origin(screenOrigin), m_zoom(zoom) {}

    void Set(Vec2 screenOrigin, float zoom) { m_origin = screenOrigin; m_zoom = zoom; }
    [[nodiscard]] Vec2 ScreenToCanvas(Vec2 screen) const {
        return m_zoom > 0.0f ? Vec2{(screen.x - m_origin.x) / m_zoom,
                                   (screen.y - m_origin.y) / m_zoom} : Vec2{};
    }
    [[nodiscard]] Vec2 CanvasToScreen(Vec2 canvas) const {
        return {m_origin.x + canvas.x * m_zoom, m_origin.y + canvas.y * m_zoom};
    }
    [[nodiscard]] Rect CanvasRectToScreen(Rect canvas) const {
        const Vec2 position = CanvasToScreen({canvas.x, canvas.y});
        return {position.x, position.y, canvas.w * m_zoom, canvas.h * m_zoom};
    }
    [[nodiscard]] Rect ScreenRectToCanvas(Rect screen) const {
        const Vec2 position = ScreenToCanvas({screen.x, screen.y});
        return {position.x, position.y, screen.w / m_zoom, screen.h / m_zoom};
    }
    [[nodiscard]] float ScreenLengthToCanvas(float value) const {
        return m_zoom > 0.0f ? value / m_zoom : 0.0f;
    }
    [[nodiscard]] float Zoom() const { return m_zoom; }
    [[nodiscard]] Vec2 Origin() const { return m_origin; }

private:
    Vec2 m_origin{};
    float m_zoom = 1.0f;
};

}  // namespace px::editor
