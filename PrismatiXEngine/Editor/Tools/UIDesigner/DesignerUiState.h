#pragma once

#include "Editor/Tools/UIDesigner/UIDesignerSession.h"

namespace px::editor {

enum class DesignerSelectionPresentation { NoDocument, None, Single, Multiple };

struct DesignerUiStateSnapshot {
    DesignerSelectionPresentation selection = DesignerSelectionPresentation::NoDocument;
    Uuid primary;
    std::size_t selectionCount = 0;
    ui::ChildLayoutPolicy parentPolicy = ui::ChildLayoutPolicy::Free;
    bool positionManaged = false;
};

struct DesignerUiRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    auto operator<=>(const DesignerUiRect&) const = default;
};

struct DesignerWorkspaceGeometryInput {
    float width = 0.0f;
    float height = 0.0f;
    float leftPanelWidth = 260.0f;
    float rightPanelWidth = 340.0f;
    float bottomPanelHeight = 240.0f;
    bool composite = true;
    bool leftPanelVisible = true;
    bool rightPanelVisible = true;
    bool bottomPanelVisible = false;
    bool clipTimeline = false;
};

struct DesignerWorkspaceGeometry {
    bool compact = false;
    bool showLeft = false;
    bool showRight = false;
    bool showBottom = false;
    DesignerUiRect main;
    DesignerUiRect left;
    DesignerUiRect canvas;
    DesignerUiRect inspector;
    DesignerUiRect bottomSplitter;
    DesignerUiRect bottomDrawer;
};

[[nodiscard]] DesignerUiStateSnapshot CaptureDesignerUiState(
    const UIDesignerSession& session);
[[nodiscard]] DesignerWorkspaceGeometry CalculateDesignerWorkspaceGeometry(
    const DesignerWorkspaceGeometryInput& input);

}  // namespace px::editor
