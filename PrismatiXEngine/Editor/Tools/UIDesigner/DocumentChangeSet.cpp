#include "Editor/Tools/UIDesigner/DocumentChangeSet.h"

#include <algorithm>
#include <array>

namespace px::editor {
namespace {

template <typename T>
void AppendUnique(std::vector<T>& target, const T& value) {
    if (std::find(target.begin(), target.end(), value) == target.end()) target.push_back(value);
}

}  // namespace

DocumentChangeSet DocumentChangeSet::Property(Uuid node, std::string property,
                                               DesignerDirtyFlags dirty) {
    DocumentChangeSet result;
    result.dirty = dirty;
    result.nodes.push_back(node);
    result.properties.push_back({node, std::move(property)});
    return result;
}

DocumentChangeSet DocumentChangeSet::Structure(Uuid root) {
    DocumentChangeSet result;
    result.dirty = DesignerDirtyFlags::Structure | DesignerDirtyFlags::Layout |
                   DesignerDirtyFlags::Paint;
    result.structuralRoots.push_back(root);
    if (!root.Empty()) result.nodes.push_back(root);
    return result;
}

DocumentChangeSet DocumentChangeSet::WholeDocument(DesignerDirtyFlags dirty,
                                                    bool historyNavigation) {
    DocumentChangeSet result;
    result.dirty = dirty;
    result.historyNavigation = historyNavigation;
    if (HasDirtyFlag(dirty, DesignerDirtyFlags::Structure)) result.structuralRoots.push_back({});
    return result;
}

void DocumentChangeSet::Merge(const DocumentChangeSet& other) {
    dirty |= other.dirty;
    historyNavigation = historyNavigation || other.historyNavigation;
    for (const auto& node : other.nodes) AppendUnique(nodes, node);
    for (const auto& property : other.properties) AppendUnique(properties, property);
    for (const auto& root : other.structuralRoots) AppendUnique(structuralRoots, root);
}

DesignerUpdate PlanDesignerUpdate(const DocumentChangeSet& changes) {
    DesignerUpdate result = DesignerUpdate::None;
    if (HasDirtyFlag(changes.dirty, DesignerDirtyFlags::Structure))
        result |= DesignerUpdate::RebuildIndex |
                  DesignerUpdate::RebuildLayoutScene |
                  DesignerUpdate::Relayout;
    else if (HasDirtyFlag(changes.dirty, DesignerDirtyFlags::Layout))
        result |= DesignerUpdate::PatchLayoutProperties | DesignerUpdate::Relayout;
    if (HasDirtyFlag(changes.dirty, DesignerDirtyFlags::Paint))
        result |= DesignerUpdate::Repaint;
    if (HasDirtyFlag(changes.dirty, DesignerDirtyFlags::Theme))
        result |= DesignerUpdate::InvalidateStyle;
    if (HasDirtyFlag(changes.dirty, DesignerDirtyFlags::Binding))
        result |= DesignerUpdate::ReconnectBindings;
    if (HasDirtyFlag(changes.dirty, DesignerDirtyFlags::Animation))
        result |= DesignerUpdate::UpdateAnimations;
    return result;
}

bool IsLayoutAffectingStyleProperty(std::string_view property) {
    constexpr std::array<std::string_view, 4> geometryProperties{
        "padding", "spacing", "typography.font", "typography.size"};
    return std::find(geometryProperties.begin(), geometryProperties.end(), property) !=
           geometryProperties.end();
}

}  // namespace px::editor
