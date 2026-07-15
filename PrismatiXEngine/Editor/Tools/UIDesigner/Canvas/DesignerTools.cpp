#include "Editor/Tools/UIDesigner/Canvas/DesignerTools.h"

namespace px::editor {

bool SelectTool::PointerDown(const DesignerPointerEvent& event) {
    m_active = event.button == DesignerMouseButton::Left;
    return m_active;
}

bool SelectTool::PointerMove(const DesignerPointerEvent&) { return m_active; }

bool SelectTool::PointerUp(const DesignerPointerEvent&) {
    const bool handled = m_active;
    m_active = false;
    return handled;
}

void SelectTool::Cancel() { m_active = false; }

bool AnchorTool::PointerDown(const DesignerPointerEvent& event) {
    m_active = event.button == DesignerMouseButton::Left;
    return m_active;
}

bool AnchorTool::PointerMove(const DesignerPointerEvent&) { return m_active; }

bool AnchorTool::PointerUp(const DesignerPointerEvent&) {
    const bool handled = m_active;
    m_active = false;
    return handled;
}

void AnchorTool::Cancel() { m_active = false; }

}  // namespace px::editor
