#include "Editor/Tools/UIDesigner/DesignerDocumentView.h"

#include <utility>

namespace px::editor {
namespace {

diag::Diagnostic ViewError(std::string code, std::string message, const Uuid& node = {}) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                                .code = std::move(code),
                                .category = "Editor.UIDesigner.DocumentView",
                                .message = std::move(message)};
    if (!node.Empty()) diagnostic.source.nodeId = node.ToString();
    return diagnostic;
}

}  // namespace

Status DesignerDocumentView::Rebuild(UISceneDocument& document) {
    std::unordered_map<Uuid, std::size_t, UuidHash> nodeIndex;
    std::unordered_map<Uuid, std::vector<Uuid>, UuidHash> children;
    std::unordered_map<Uuid, Uuid, UuidHash> parent;
    std::unordered_map<Uuid, std::size_t, UuidHash> childIndex;
    Uuid root;

    auto& nodes = document.Data().nodes;
    nodeIndex.reserve(nodes.size());
    parent.reserve(nodes.size());
    childIndex.reserve(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        if (node.id.Empty() || !nodeIndex.emplace(node.id, index).second) {
            return Status::Fail(ViewError("PXEDUI-VIEW-0001",
                                          "UI document contains an empty or duplicate node UUID",
                                          node.id));
        }
        parent.emplace(node.id, node.parent);
        if (node.parent.Empty()) {
            if (!root.Empty()) {
                return Status::Fail(ViewError("PXEDUI-VIEW-0002",
                                              "UI document contains more than one root node",
                                              node.id));
            }
            root = node.id;
        }
    }
    if (!nodes.empty() && root.Empty()) {
        return Status::Fail(ViewError("PXEDUI-VIEW-0003", "UI document has no root node"));
    }

    for (const auto& node : nodes) {
        if (!node.parent.Empty() && !nodeIndex.contains(node.parent)) {
            return Status::Fail(ViewError("PXEDUI-VIEW-0004",
                                          "UI node references a missing parent", node.id));
        }
        auto& siblings = children[node.parent];
        childIndex.emplace(node.id, siblings.size());
        siblings.push_back(node.id);
    }

    // Validate parent chains while the temporary maps are still isolated from
    // the last known-good index.
    for (const auto& node : nodes) {
        Uuid current = node.parent;
        std::size_t guard = 0;
        while (!current.Empty()) {
            if (current == node.id || ++guard > nodes.size()) {
                return Status::Fail(ViewError("PXEDUI-VIEW-0005",
                                              "UI document contains a parent cycle", node.id));
            }
            const auto found = parent.find(current);
            current = found == parent.end() ? Uuid{} : found->second;
        }
    }

    std::erase_if(m_layoutRects, [&](const auto& entry) { return !nodeIndex.contains(entry.first); });
    std::erase_if(m_childPolicies, [&](const auto& entry) { return !nodeIndex.contains(entry.first); });
    m_nodeIndex = std::move(nodeIndex);
    m_children = std::move(children);
    m_parent = std::move(parent);
    m_childIndex = std::move(childIndex);
    m_root = root;
    ++m_revision;
    return Status::Ok();
}

void DesignerDocumentView::Clear() {
    m_nodeIndex.clear();
    m_children.clear();
    m_parent.clear();
    m_childIndex.clear();
    m_layoutRects.clear();
    m_childPolicies.clear();
    m_root = {};
    ++m_revision;
}

resource::NodeRecord* DesignerDocumentView::Find(UISceneDocument& document, const Uuid& id) const {
    const auto found = m_nodeIndex.find(id);
    if (found == m_nodeIndex.end() || found->second >= document.Data().nodes.size()) return nullptr;
    auto& node = document.Data().nodes[found->second];
    return node.id == id ? &node : nullptr;
}

const resource::NodeRecord* DesignerDocumentView::Find(const UISceneDocument& document,
                                                        const Uuid& id) const {
    const auto found = m_nodeIndex.find(id);
    if (found == m_nodeIndex.end() || found->second >= document.Data().nodes.size()) return nullptr;
    const auto& node = document.Data().nodes[found->second];
    return node.id == id ? &node : nullptr;
}

bool DesignerDocumentView::Contains(const Uuid& id) const { return m_nodeIndex.contains(id); }

std::optional<std::size_t> DesignerDocumentView::NodeIndex(const Uuid& id) const {
    const auto found = m_nodeIndex.find(id);
    return found == m_nodeIndex.end() ? std::nullopt
                                     : std::optional<std::size_t>(found->second);
}

std::span<const Uuid> DesignerDocumentView::Children(const Uuid& parent) const {
    const auto found = m_children.find(parent);
    return found == m_children.end() ? std::span<const Uuid>{}
                                     : std::span<const Uuid>{found->second};
}

Uuid DesignerDocumentView::Parent(const Uuid& child) const {
    const auto found = m_parent.find(child);
    return found == m_parent.end() ? Uuid{} : found->second;
}

std::optional<std::size_t> DesignerDocumentView::ChildIndex(const Uuid& child) const {
    const auto found = m_childIndex.find(child);
    return found == m_childIndex.end() ? std::nullopt
                                      : std::optional<std::size_t>(found->second);
}

bool DesignerDocumentView::IsAncestor(const Uuid& ancestor, const Uuid& descendant) const {
    if (ancestor.Empty() || descendant.Empty() || ancestor == descendant) return false;
    Uuid current = Parent(descendant);
    std::size_t guard = 0;
    while (!current.Empty() && guard++ <= m_parent.size()) {
        if (current == ancestor) return true;
        current = Parent(current);
    }
    return false;
}

bool DesignerDocumentView::IsWithin(const Uuid& scope, const Uuid& node) const {
    return scope.Empty() || scope == node || IsAncestor(scope, node);
}

void DesignerDocumentView::SetLayoutRect(const Uuid& id, Rect rect) {
    if (Contains(id)) m_layoutRects[id] = rect;
}

void DesignerDocumentView::SetChildPolicy(const Uuid& id, ui::ChildLayoutPolicy policy) {
    if (Contains(id)) m_childPolicies[id] = policy;
}

void DesignerDocumentView::ReplaceLayout(
    std::unordered_map<Uuid, Rect, UuidHash> rects,
    std::unordered_map<Uuid, ui::ChildLayoutPolicy, UuidHash> policies) {
    std::erase_if(rects, [&](const auto& entry) { return !Contains(entry.first); });
    std::erase_if(policies, [&](const auto& entry) { return !Contains(entry.first); });
    m_layoutRects = std::move(rects);
    m_childPolicies = std::move(policies);
}

void DesignerDocumentView::ClearLayout() {
    m_layoutRects.clear();
    m_childPolicies.clear();
}

std::optional<Rect> DesignerDocumentView::LayoutRect(const Uuid& id) const {
    const auto found = m_layoutRects.find(id);
    return found == m_layoutRects.end() ? std::nullopt : std::optional<Rect>(found->second);
}

std::optional<ui::ChildLayoutPolicy> DesignerDocumentView::ChildPolicy(const Uuid& id) const {
    const auto found = m_childPolicies.find(id);
    return found == m_childPolicies.end()
               ? std::nullopt
               : std::optional<ui::ChildLayoutPolicy>(found->second);
}

DesignerLayoutSnapshot DesignerDocumentView::CaptureLayout() const {
    return {.rects = m_layoutRects, .policies = m_childPolicies};
}

void DesignerDocumentView::RestoreLayout(DesignerLayoutSnapshot snapshot) {
    std::erase_if(snapshot.rects, [&](const auto& entry) { return !Contains(entry.first); });
    std::erase_if(snapshot.policies, [&](const auto& entry) { return !Contains(entry.first); });
    m_layoutRects = std::move(snapshot.rects);
    m_childPolicies = std::move(snapshot.policies);
}

}  // namespace px::editor
