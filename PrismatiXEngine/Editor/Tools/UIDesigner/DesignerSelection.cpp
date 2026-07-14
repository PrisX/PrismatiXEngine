#include "Editor/Tools/UIDesigner/DesignerSelection.h"

#include "Editor/Tools/UIDesigner/DesignerDocumentView.h"

#include <algorithm>

namespace px::editor {

void DesignerSelection::Replace(const Uuid& id) {
    Clear();
    if (id.Empty()) return;
    m_ordered.push_back(id);
    m_members.insert(id);
    m_primary = id;
}

void DesignerSelection::Replace(std::vector<Uuid> ids, const Uuid& primary) {
    Clear();
    m_ordered.reserve(ids.size());
    for (const Uuid& id : ids) {
        if (!id.Empty() && m_members.insert(id).second) m_ordered.push_back(id);
    }
    if (!primary.Empty() && m_members.contains(primary)) m_primary = primary;
    else ChooseFallbackPrimary();
}

bool DesignerSelection::Toggle(const Uuid& id) {
    if (id.Empty()) return false;
    if (Contains(id)) {
        Remove(id);
        return false;
    }
    Add(id, true);
    return true;
}

bool DesignerSelection::Add(const Uuid& id, bool makePrimary) {
    if (id.Empty()) return false;
    const bool inserted = m_members.insert(id).second;
    if (inserted) m_ordered.push_back(id);
    if (makePrimary) m_primary = id;
    return inserted;
}

bool DesignerSelection::Remove(const Uuid& id) {
    if (!m_members.erase(id)) return false;
    std::erase(m_ordered, id);
    if (m_primary == id) ChooseFallbackPrimary();
    return true;
}

bool DesignerSelection::SetPrimary(const Uuid& id) {
    if (!Contains(id)) return false;
    m_primary = id;
    return true;
}

void DesignerSelection::Clear() {
    m_ordered.clear();
    m_members.clear();
    m_primary = {};
}

bool DesignerSelection::SetScope(const Uuid& scope, const DesignerDocumentView& view) {
    if (!scope.Empty() && !view.Contains(scope)) return false;
    m_scope = scope;
    Prune(view);
    return true;
}

bool DesignerSelection::ExitScope(const DesignerDocumentView& view) {
    if (m_scope.Empty()) return false;
    m_scope = view.Parent(m_scope);
    Prune(view);
    return true;
}

void DesignerSelection::Prune(const DesignerDocumentView& view) {
    if (!m_scope.Empty() && !view.Contains(m_scope)) m_scope = {};
    const Uuid previousPrimary = m_primary;
    std::erase_if(m_ordered, [&](const Uuid& id) {
        if (!view.Contains(id) || !view.IsWithin(m_scope, id)) {
            m_members.erase(id);
            return true;
        }
        return false;
    });
    if (!m_members.contains(previousPrimary)) ChooseFallbackPrimary();
}

void DesignerSelection::Canonicalize(const DesignerDocumentView& view) {
    const Uuid previousPrimary = m_primary;
    Prune(view);
    if (m_ordered.size() < 2) return;

    std::vector<Uuid> canonical;
    canonical.reserve(m_ordered.size());
    std::unordered_set<Uuid, UuidHash> canonicalMembers;
    for (const Uuid& id : m_ordered) {
        Uuid ancestor = view.Parent(id);
        bool covered = false;
        std::size_t guard = 0;
        while (!ancestor.Empty() && guard++ <= view.NodeCount()) {
            if (m_members.contains(ancestor)) {
                covered = true;
                break;
            }
            ancestor = view.Parent(ancestor);
        }
        if (!covered) {
            canonical.push_back(id);
            canonicalMembers.insert(id);
        }
    }
    m_ordered = std::move(canonical);
    m_members = std::move(canonicalMembers);

    if (m_members.contains(previousPrimary)) {
        m_primary = previousPrimary;
        return;
    }
    Uuid ancestor = view.Parent(previousPrimary);
    std::size_t guard = 0;
    while (!ancestor.Empty() && guard++ <= view.NodeCount()) {
        if (m_members.contains(ancestor)) {
            m_primary = ancestor;
            return;
        }
        ancestor = view.Parent(ancestor);
    }
    ChooseFallbackPrimary();
}

void DesignerSelection::ChooseFallbackPrimary() {
    m_primary = m_ordered.empty() ? Uuid{} : m_ordered.back();
}

}  // namespace px::editor
