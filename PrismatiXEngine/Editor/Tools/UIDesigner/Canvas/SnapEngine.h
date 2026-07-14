#pragma once

#include "Editor/Tools/UIDesigner/DesignerDocumentView.h"

#include <span>
#include <vector>

namespace px::editor {

enum class SnapMode { Move, Resize };
enum class SnapGuideOrientation { Vertical, Horizontal };
enum class SnapGuideKind { Canvas, Parent, Sibling, EqualSpacing, Grid, User };

struct SnapGuide {
    SnapGuideOrientation orientation = SnapGuideOrientation::Vertical;
    SnapGuideKind kind = SnapGuideKind::Canvas;
    float position = 0.0f;
    float from = 0.0f;
    float to = 0.0f;
};

struct SnapDistanceLabel {
    Vec2 position{};
    float distance = 0.0f;
    bool horizontal = true;
};

struct UserSnapGuide {
    SnapGuideOrientation orientation = SnapGuideOrientation::Vertical;
    float position = 0.0f;
    bool locked = false;
};

struct SnapRequest {
    Rect movingRect{};
    SnapMode mode = SnapMode::Move;
    Uuid parent;
    std::span<const Uuid> ignoredNodes;
    float zoom = 1.0f;
    Rect canvasRect{};
    bool gridEnabled = false;
    float gridSize = 16.0f;
    bool snapLeft = true;
    bool snapRight = true;
    bool snapTop = true;
    bool snapBottom = true;
    std::span<const UserSnapGuide> userGuides;
};

struct SnapResult {
    Rect rect{};
    std::vector<SnapGuide> guides;
    std::vector<SnapDistanceLabel> distances;
};

class SnapEngine {
public:
    [[nodiscard]] SnapResult Snap(const SnapRequest& request,
                                  const DesignerDocumentView& view) const;
};

}  // namespace px::editor
