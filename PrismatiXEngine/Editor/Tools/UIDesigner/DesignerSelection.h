#pragma once

#include "Engine/Core/Uuid.h"

#include <span>
#include <unordered_set>
#include <vector>

namespace px::editor {

class DesignerDocumentView;

class DesignerSelection {
public:
    [[nodiscard]] Uuid Primary() const { return m_primary; }
    [[nodiscard]] std::span<const Uuid> OrderedItems() const { return m_ordered; }
    [[nodiscard]] bool Empty() const { return m_ordered.empty(); }
    [[nodiscard]] std::size_t Size() const { return m_ordered.size(); }
    [[nodiscard]] bool Contains(const Uuid& id) const { return m_members.contains(id); }

    void Replace(const Uuid& id);
    void Replace(std::vector<Uuid> ids, const Uuid& primary);
    bool Toggle(const Uuid& id);
    bool Add(const Uuid& id, bool makePrimary = true);
    bool Remove(const Uuid& id);
    bool SetPrimary(const Uuid& id);
    void Clear();

    [[nodiscard]] Uuid Scope() const { return m_scope; }
    bool SetScope(const Uuid& scope, const DesignerDocumentView& view);
    void ClearScope() { m_scope = {}; }
    bool ExitScope(const DesignerDocumentView& view);

    // Removes stale/out-of-scope IDs and ancestor/descendant duplicates while
    // preserving deterministic selection order.
    void Canonicalize(const DesignerDocumentView& view);
    void Prune(const DesignerDocumentView& view);

private:
    void ChooseFallbackPrimary();

    std::vector<Uuid> m_ordered;
    std::unordered_set<Uuid, UuidHash> m_members;
    Uuid m_primary;
    Uuid m_scope;
};

}  // namespace px::editor
