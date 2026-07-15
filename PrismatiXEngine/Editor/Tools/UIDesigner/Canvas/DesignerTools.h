#pragma once

#include "Editor/Tools/UIDesigner/Canvas/CanvasInteractionController.h"

namespace px::editor {

// These tools deliberately own only pointer-capture lifetime. Selection,
// authored data and gesture state remain in UIDesignerSession and are updated
// by the command-backed canvas handlers after the controller accepts an event.
class SelectTool final : public IDesignerTool {
public:
    bool PointerDown(const DesignerPointerEvent& event) override;
    bool PointerMove(const DesignerPointerEvent& event) override;
    bool PointerUp(const DesignerPointerEvent& event) override;
    void Cancel() override;

private:
    bool m_active = false;
};

class AnchorTool final : public IDesignerTool {
public:
    bool PointerDown(const DesignerPointerEvent& event) override;
    bool PointerMove(const DesignerPointerEvent& event) override;
    bool PointerUp(const DesignerPointerEvent& event) override;
    void Cancel() override;

private:
    bool m_active = false;
};

}  // namespace px::editor
