#include "Editor/Tools/UIDesigner/DesignerUiState.h"

#include <algorithm>

namespace px::editor {

DesignerUiStateSnapshot CaptureDesignerUiState(const UIDesignerSession& session) {
    DesignerUiStateSnapshot result;
    if (!session.HasDocument()) return result;
    result.primary = session.Selection().Primary();
    result.selectionCount = session.Selection().Size();
    result.selection = result.selectionCount == 0
                           ? DesignerSelectionPresentation::None
                           : (result.selectionCount == 1
                                  ? DesignerSelectionPresentation::Single
                                  : DesignerSelectionPresentation::Multiple);
    const auto* document = session.Document();
    const auto* node = document && !result.primary.Empty()
                           ? session.DocumentView().Find(*document, result.primary)
                           : nullptr;
    if (node && !node->parent.Empty()) {
        result.parentPolicy = session.DocumentView().ChildPolicy(node->parent)
                                  .value_or(ui::ChildLayoutPolicy::Free);
        result.positionManaged = result.parentPolicy != ui::ChildLayoutPolicy::Free;
    }
    return result;
}

DesignerWorkspaceGeometry CalculateDesignerWorkspaceGeometry(
    const DesignerWorkspaceGeometryInput& input) {
    constexpr float kCompactWidth = 1000.0f;
    constexpr float kMinimumCanvasWidth = 420.0f;
    constexpr float kMinimumMainHeight = 240.0f;
    constexpr float kMinimumDrawerHeight = 150.0f;
    constexpr float kSplitterHeight = 6.0f;

    DesignerWorkspaceGeometry result;
    const float width = std::max(0.0f, input.width);
    const float height = std::max(0.0f, input.height);
    const float leftWidth = std::clamp(input.leftPanelWidth, 180.0f, width);
    const float rightWidth = std::clamp(input.rightPanelWidth, 240.0f, width);

    result.compact = width < kCompactWidth;
    result.showLeft = input.composite && input.leftPanelVisible && !result.compact;
    const float widthAfterLeft = width - (result.showLeft ? leftWidth : 0.0f);
    result.showRight = input.composite && input.rightPanelVisible &&
                       widthAfterLeft - rightWidth >= kMinimumCanvasWidth;
    result.showBottom = input.composite &&
                        (input.clipTimeline || input.bottomPanelVisible) &&
                        height >= kMinimumMainHeight + kMinimumDrawerHeight +
                                      kSplitterHeight;

    float drawerHeight = 0.0f;
    if (result.showBottom) {
        const float maximumDrawerHeight =
            std::max(kMinimumDrawerHeight,
                     std::min(height * 0.5f,
                              height - kMinimumMainHeight - kSplitterHeight));
        drawerHeight = std::clamp(input.bottomPanelHeight,
                                  kMinimumDrawerHeight, maximumDrawerHeight);
    }
    const float mainHeight =
        std::max(0.0f, height - drawerHeight -
                           (result.showBottom ? kSplitterHeight : 0.0f));
    result.main = {0.0f, 0.0f, width, mainHeight};

    float canvasX = 0.0f;
    if (result.showLeft) {
        result.left = {0.0f, 0.0f, leftWidth, mainHeight};
        canvasX += leftWidth;
    }
    const float inspectorWidth = result.showRight ? rightWidth : 0.0f;
    result.canvas = {canvasX, 0.0f,
                     std::max(0.0f, width - canvasX - inspectorWidth), mainHeight};
    if (result.showRight) {
        result.inspector = {width - rightWidth, 0.0f, rightWidth, mainHeight};
    }
    if (result.showBottom) {
        result.bottomSplitter = {0.0f, mainHeight, width, kSplitterHeight};
        result.bottomDrawer = {0.0f, mainHeight + kSplitterHeight,
                               width, drawerHeight};
    }
    return result;
}

}  // namespace px::editor
