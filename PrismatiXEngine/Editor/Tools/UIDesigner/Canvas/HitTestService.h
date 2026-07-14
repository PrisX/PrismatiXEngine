#pragma once

#include "Editor/Tools/UIDesigner/DesignerDocumentView.h"

#include <vector>

namespace px::editor {

class HitTestService {
public:
    [[nodiscard]] std::vector<Uuid> HitStack(const UISceneDocument& document,
                                             const DesignerDocumentView& view,
                                             Vec2 canvasPosition,
                                             const Uuid& scope = {}) const;
    [[nodiscard]] Uuid Topmost(const UISceneDocument& document,
                               const DesignerDocumentView& view, Vec2 canvasPosition,
                               const Uuid& scope = {}) const;
};

}  // namespace px::editor
