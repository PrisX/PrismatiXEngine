#include "Editor/Tools/UIDesigner/Canvas/CanvasInteractionController.h"

namespace px::editor {

void CanvasInteractionController::SetActiveTool(IDesignerTool* tool) {
    if (m_activeTool == tool) return;
    Cancel();
    m_activeTool = tool;
}

bool CanvasInteractionController::PointerDown(const DesignerPointerEvent& event) {
    if (!m_activeTool || m_capturedTool) return false;
    if (!m_activeTool->PointerDown(event)) return false;
    m_capturedTool = m_activeTool;
    return true;
}

bool CanvasInteractionController::PointerMove(const DesignerPointerEvent& event) {
    IDesignerTool* target = m_capturedTool ? m_capturedTool : m_activeTool;
    return target && target->PointerMove(event);
}

bool CanvasInteractionController::PointerUp(const DesignerPointerEvent& event) {
    IDesignerTool* target = m_capturedTool ? m_capturedTool : m_activeTool;
    if (!target) return false;
    const bool handled = target->PointerUp(event);
    m_capturedTool = nullptr;
    return handled;
}

void CanvasInteractionController::Cancel() {
    if (m_capturedTool) m_capturedTool->Cancel();
    m_capturedTool = nullptr;
}

}  // namespace px::editor
