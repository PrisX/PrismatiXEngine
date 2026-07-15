#pragma once

#include "Engine/Core/Types.h"

#include <cstdint>

namespace px::editor {

enum class DesignerMouseButton : std::uint8_t { None, Left, Right };

struct DesignerModifierKeys {
    bool shift = false;
    bool alt = false;
    bool controlOrCommand = false;
};

struct DesignerPointerEvent {
    Vec2 screenPosition{};
    Vec2 canvasPosition{};
    float zoom = 1.0f;
    DesignerMouseButton button = DesignerMouseButton::None;
    DesignerModifierKeys modifiers{};
    std::uint32_t clickCount = 1;
};

class IDesignerTool {
public:
    virtual ~IDesignerTool() = default;
    virtual bool PointerDown(const DesignerPointerEvent&) = 0;
    virtual bool PointerMove(const DesignerPointerEvent&) = 0;
    virtual bool PointerUp(const DesignerPointerEvent&) = 0;
    virtual void Cancel() = 0;
};

class CanvasInteractionController {
public:
    void SetActiveTool(IDesignerTool* tool);
    [[nodiscard]] IDesignerTool* ActiveTool() const { return m_activeTool; }
    [[nodiscard]] IDesignerTool* CapturedTool() const { return m_capturedTool; }
    [[nodiscard]] bool HasCapture() const { return m_capturedTool != nullptr; }

    bool PointerDown(const DesignerPointerEvent& event);
    bool PointerMove(const DesignerPointerEvent& event);
    bool PointerUp(const DesignerPointerEvent& event);
    void Cancel();

private:
    IDesignerTool* m_activeTool = nullptr;
    IDesignerTool* m_capturedTool = nullptr;
};

}  // namespace px::editor
