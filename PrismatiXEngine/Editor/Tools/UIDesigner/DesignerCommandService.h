#pragma once

#include "Editor/Tools/UIDesigner/UIDesignerSession.h"

#include <functional>
#include <memory>
#include <span>
#include <string>

namespace px::editor {

class DesignerCommandService {
public:
    explicit DesignerCommandService(UIDesignerSession& session) : m_session(session) {}

    using Changed = std::function<void(DesignerDirtyFlags)>;
    void SetChanged(Changed changed) { m_changed = std::move(changed); }

    Status Execute(std::unique_ptr<EditCommand> command, DesignerDirtyFlags dirty,
                   bool structural = false);
    Status CommitApplied(std::unique_ptr<EditCommand> command, DesignerDirtyFlags dirty,
                         bool structural = false);
    Status Undo();
    Status Redo();

    Status SetProperty(const Uuid& target, std::string property, Variant value,
                       std::string label = {},
                       DesignerDirtyFlags dirty = DesignerDirtyFlags::Paint);
    Status SetProperties(std::span<const Uuid> targets, const std::string& property,
                         const Variant& value, std::string label,
                         DesignerDirtyFlags dirty = DesignerDirtyFlags::Paint);
    Status Rename(const Uuid& target, std::string name);
    Status SetVisibility(const Uuid& target, std::string visibility);
    Status SetLocked(const Uuid& target, bool locked);
    Status DeleteSelection();
    Status DuplicateSelection();
    Status Reparent(const Uuid& target, const Uuid& newParent, std::size_t newIndex);
    Status Reorder(const Uuid& target, std::size_t newIndex);

    Status BeginPropertyGesture(const Uuid& target, std::string property, std::string label,
                                DesignerDirtyFlags dirty);
    Status UpdatePropertyGesture(Variant value);
    Status CommitPropertyGesture();
    Status CancelPropertyGesture();
    [[nodiscard]] bool GestureActive() const {
        return m_transaction && m_transaction->Active();
    }

private:
    Status After(DesignerDirtyFlags dirty, bool structural);
    static void RegenerateIds(VariantObject& subtree,
                              std::vector<Uuid>* generated = nullptr);

    UIDesignerSession& m_session;
    Changed m_changed;
    std::unique_ptr<PropertyEditTransaction> m_transaction;
    DesignerDirtyFlags m_transactionDirty = DesignerDirtyFlags::None;
};

}  // namespace px::editor
