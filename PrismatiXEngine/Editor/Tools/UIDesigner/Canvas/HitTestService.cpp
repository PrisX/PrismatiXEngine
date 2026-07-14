#include "Editor/Tools/UIDesigner/Canvas/HitTestService.h"

namespace px::editor {

std::vector<Uuid> HitTestService::HitStack(const UISceneDocument& document,
                                           const DesignerDocumentView& view,
                                           Vec2 canvasPosition, const Uuid& scope) const {
    std::vector<Uuid> result;
    // Canonical node vector order is sibling draw order; reverse iteration is
    // therefore the unique topmost-to-back hit order used by Runtime and Layers.
    for (auto it = document.Data().nodes.rbegin(); it != document.Data().nodes.rend(); ++it) {
        if (!view.IsWithin(scope, it->id)) continue;
        const auto rect = view.LayoutRect(it->id);
        if (!rect || !rect->Contains(canvasPosition.x, canvasPosition.y)) continue;
        if (const auto locked = it->properties.find("editorLocked");
            locked != it->properties.end() && locked->second.TryGet<bool>() &&
            *locked->second.TryGet<bool>()) continue;
        if (const auto visibility = it->properties.find("visibility");
            visibility != it->properties.end() && visibility->second.TryGet<std::string>() &&
            *visibility->second.TryGet<std::string>() != "Visible") continue;
        result.push_back(it->id);
    }
    return result;
}

Uuid HitTestService::Topmost(const UISceneDocument& document,
                             const DesignerDocumentView& view, Vec2 canvasPosition,
                             const Uuid& scope) const {
    auto hits = HitStack(document, view, canvasPosition, scope);
    return hits.empty() ? Uuid{} : hits.front();
}

}  // namespace px::editor
