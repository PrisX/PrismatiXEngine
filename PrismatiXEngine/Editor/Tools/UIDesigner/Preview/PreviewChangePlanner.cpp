#include "Editor/Tools/UIDesigner/Preview/PreviewChangePlanner.h"

namespace px::editor {

PreviewUpdate PlanPreviewUpdate(const DocumentChangeSet& changes, const Uuid& documentId) {
    if (changes.Structural() || HasDirtyFlag(changes.dirty, DesignerDirtyFlags::Structure))
        return PreviewUpdate::RebuildScene;

    PreviewUpdate result = PreviewUpdate::None;
    if (HasDirtyFlag(changes.dirty, DesignerDirtyFlags::Layout))
        result |= PreviewUpdate::Relayout;
    for (const auto& changed : changes.properties) {
        if (changed.node == documentId) {
            if (changed.property == "animations") result |= PreviewUpdate::UpdateAnimations;
            else if (changed.property == "styleSystem" || changed.property == "theme")
                result |= PreviewUpdate::InvalidateStyle;
            else if (changed.property == "interactionGraph")
                result |= PreviewUpdate::ReconnectBindings;
            else
                return PreviewUpdate::RebuildScene;
            continue;
        }
        if (changed.property == "bindings" || changed.property == "triggers")
            result |= PreviewUpdate::ReconnectBindings;
        else if (changed.property == "styleBinding")
            result |= PreviewUpdate::InvalidateStyle;
        else if (changed.property == "overrides" ||
                 changed.property == "componentProperties" ||
                 changed.property == "componentEvents" ||
                 changed.property == "componentSlot")
            return PreviewUpdate::RebuildScene;
        else
            result |= PreviewUpdate::PatchProperties;
    }
    return result == PreviewUpdate::None ? PreviewUpdate::RebuildScene : result;
}

}  // namespace px::editor
