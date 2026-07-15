#pragma once

#include "Editor/Tools/UIDesigner/UIDesignerSession.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace px::editor {

class DesignerCommandService {
public:
    explicit DesignerCommandService(UIDesignerSession& session) : m_session(session) {}

    using Changed = std::function<void(const DocumentChangeSet&)>;
    void SetChanged(Changed changed) { m_changed = std::move(changed); }

    Status Execute(std::unique_ptr<EditCommand> command, DocumentChangeSet changes);
    Status Execute(std::unique_ptr<EditCommand> command, DesignerDirtyFlags dirty,
                   bool structural = false);
    Status CommitApplied(std::unique_ptr<EditCommand> command, DocumentChangeSet changes);
    Status CommitApplied(std::unique_ptr<EditCommand> command, DesignerDirtyFlags dirty,
                         bool structural = false);
    Status Undo();
    Status Redo();
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    [[nodiscard]] std::string NextUndoLabel() const;
    [[nodiscard]] std::string NextRedoLabel() const;
    [[nodiscard]] std::size_t HistoryCursor() const;
    [[nodiscard]] bool HistoryDirty() const;

    Status SetProperty(const Uuid& target, std::string property, Variant value,
                       std::string label = {},
                       DesignerDirtyFlags dirty = DesignerDirtyFlags::Paint);
    Status SetProperties(std::span<const Uuid> targets, const std::string& property,
                         const Variant& value, std::string label,
                         DesignerDirtyFlags dirty = DesignerDirtyFlags::Paint);
    Status ApplyTransientProperty(const Uuid& target, std::string property,
                                  const Variant& value, DesignerDirtyFlags dirty);
    Status Rename(const Uuid& target, std::string name);
    Status SetVisibility(const Uuid& target, std::string visibility);
    Status SetLocked(const Uuid& target, bool locked);
    Status DeleteSelection();
    Status DuplicateSelection();
    Status Reparent(const Uuid& target, const Uuid& newParent, std::size_t newIndex);
    Status Reorder(const Uuid& target, std::size_t newIndex);

    Status BeginPropertyGesture(const Uuid& target, std::string property, std::string label,
                                DesignerDirtyFlags dirty);
    Status BeginPropertyGesture(std::span<const Uuid> targets, std::string property,
                                std::string label, DesignerDirtyFlags dirty);
    Status UpdatePropertyGesture(Variant value);
    Status CommitPropertyGesture();
    Status CancelPropertyGesture();
    [[nodiscard]] bool GestureMatches(std::span<const Uuid> targets,
                                      std::string_view property) const;
    [[nodiscard]] bool GestureActive() const {
        return (m_transaction && m_transaction->Active()) ||
               (m_multiTransaction && m_multiTransaction->Active());
    }

private:
    Status After(const DocumentChangeSet& changes);
    void RecordHistoryChange(std::size_t beforeCursor, std::size_t afterCursor,
                             const DocumentChangeSet& changes);
    static void RegenerateIds(VariantObject& subtree,
                              std::vector<Uuid>* generated = nullptr);

    UIDesignerSession& m_session;
    Changed m_changed;
    std::unique_ptr<PropertyEditTransaction> m_transaction;
    std::unique_ptr<MultiPropertyEditTransaction> m_multiTransaction;
    DocumentChangeSet m_transactionChanges;
    std::vector<Uuid> m_gestureTargets;
    std::string m_gestureProperty;
    std::size_t m_gestureHistoryCursor = 0;
    std::vector<DocumentChangeSet> m_historyChanges;
};

}  // namespace px::editor
