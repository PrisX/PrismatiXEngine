#pragma once

#include "Editor/Tools/UIDesigner/UISceneDocument.h"
#include "Engine/UI/Control.h"

#include <cstddef>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace px::editor {

struct DesignerLayoutSnapshot {
    std::unordered_map<Uuid, Rect, UuidHash> rects;
    std::unordered_map<Uuid, ui::ChildLayoutPolicy, UuidHash> policies;
};

// Fast, disposable editor-side index over the canonical UISceneDocument.
// It never owns node data and is rebuilt after structural commands.
class DesignerDocumentView {
public:
    Status Rebuild(UISceneDocument& document);
    void Clear();

    [[nodiscard]] resource::NodeRecord* Find(UISceneDocument& document, const Uuid& id) const;
    [[nodiscard]] const resource::NodeRecord* Find(const UISceneDocument& document,
                                                   const Uuid& id) const;
    [[nodiscard]] bool Contains(const Uuid& id) const;
    [[nodiscard]] std::optional<std::size_t> NodeIndex(const Uuid& id) const;
    [[nodiscard]] std::span<const Uuid> Children(const Uuid& parent) const;
    [[nodiscard]] Uuid Parent(const Uuid& child) const;
    [[nodiscard]] std::optional<std::size_t> ChildIndex(const Uuid& child) const;
    [[nodiscard]] Uuid Root() const { return m_root; }
    [[nodiscard]] bool IsAncestor(const Uuid& ancestor, const Uuid& descendant) const;
    [[nodiscard]] bool IsWithin(const Uuid& scope, const Uuid& node) const;

    void SetLayoutRect(const Uuid& id, Rect rect);
    void SetChildPolicy(const Uuid& id, ui::ChildLayoutPolicy policy);
    void ReplaceLayout(std::unordered_map<Uuid, Rect, UuidHash> rects,
                       std::unordered_map<Uuid, ui::ChildLayoutPolicy, UuidHash> policies);
    void ClearLayout();
    [[nodiscard]] std::optional<Rect> LayoutRect(const Uuid& id) const;
    [[nodiscard]] std::optional<ui::ChildLayoutPolicy> ChildPolicy(const Uuid& id) const;
    [[nodiscard]] const std::unordered_map<Uuid, Rect, UuidHash>& LayoutRects() const {
        return m_layoutRects;
    }
    [[nodiscard]] DesignerLayoutSnapshot CaptureLayout() const;
    void RestoreLayout(DesignerLayoutSnapshot snapshot);

    [[nodiscard]] std::size_t NodeCount() const { return m_nodeIndex.size(); }
    [[nodiscard]] std::uint64_t Revision() const { return m_revision; }

private:
    std::unordered_map<Uuid, std::size_t, UuidHash> m_nodeIndex;
    std::unordered_map<Uuid, std::vector<Uuid>, UuidHash> m_children;
    std::unordered_map<Uuid, Uuid, UuidHash> m_parent;
    std::unordered_map<Uuid, std::size_t, UuidHash> m_childIndex;
    std::unordered_map<Uuid, Rect, UuidHash> m_layoutRects;
    std::unordered_map<Uuid, ui::ChildLayoutPolicy, UuidHash> m_childPolicies;
    Uuid m_root;
    std::uint64_t m_revision = 0;
};

}  // namespace px::editor
